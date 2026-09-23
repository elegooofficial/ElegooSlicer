#include "PrinterMmsManager.hpp"
#include "slic3r/Utils/Elegoo/PrinterManager.hpp"
#include "libslic3r/StandardColorMatcher.hpp"
#include <nlohmann/json.hpp>
#include <wx/colour.h>
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem.hpp>
#include <mutex>
#include <algorithm>
#include <limits>

namespace Slic3r {

// Legacy field names kept so JSON mapping keys and log lines elsewhere in
// this file remain stable.
struct StandardColor
{
    std::string colorName;
    std::string colorHex;
};

static StandardColor getStandardColor(const std::string& hex)
{
    static const StandardColorMatcher matcher;
    if (auto m = matcher.match(hex))
        return {m->name, m->hex};
    return {};
}

// standardize filament name for matching: remove vendor/generic prefixes, replace dashes with spaces
static std::string standardizeFilamentName(const std::string& name, const std::string& vendor)
{
    std::string standardized = boost::to_upper_copy(name);
    boost::trim(standardized);
    
    // remove vendor prefix if exists
    std::string vendorUpper = boost::to_upper_copy(vendor);
    boost::trim(vendorUpper);
    if(!vendorUpper.empty() && standardized.find(vendorUpper) != std::string::npos) {
        boost::erase_all(standardized, vendorUpper);
        boost::trim(standardized);
    }
    
    // remove GENERIC prefix if exists
    if(standardized.find("GENERIC") != std::string::npos) {
        boost::erase_all(standardized, "GENERIC");
        boost::trim(standardized);
    }
    
    // remove @ suffix if exists
    size_t atPos = standardized.find('@');
    if(atPos != std::string::npos) {
        standardized = standardized.substr(0, atPos);
        boost::trim(standardized);
    }
    
    // replace all dashes with spaces
    std::replace(standardized.begin(), standardized.end(), '-', ' ');
    boost::trim(standardized);
    
    return standardized;
}

PrinterMmsManager::PrinterMmsManager() {}

PrinterMmsManager::~PrinterMmsManager() {}


// find mms tray filament id by matching standardized filament name
// builds a merged preset map containing vendor specific, vendor generic, and orca generic filaments
// matches by standardized name (removes vendor/generic prefixes, replaces dashes with spaces)
// if multiple candidates found, selects the one with highest priority (lowest priority number)

void PrinterMmsManager::getMmsTrayFilamentId(const PrinterNetworkInfo& printerNetworkInfo, PrinterMmsGroup& mmsGroup)
{
    std::vector<double> currentProjectNozzleDiameters = {0.4};
    try {
        auto& app = GUI::wxGetApp();
        if(app.preset_bundle) {
            auto nozzleDiameterOpt = app.preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
            if(nozzleDiameterOpt && !nozzleDiameterOpt->values.empty()) {
                currentProjectNozzleDiameters = nozzleDiameterOpt->values;
            }
        }
    } catch(...) {
    }
    
    // build merged preset map with all filament types (vendor specific, vendor generic, orca generic)
    auto mergedPresetMap = buildPresetFilamentMap(printerNetworkInfo, currentProjectNozzleDiameters);

    // match filament id in system preset to mms tray by standardized name
    // if multiple candidates found, select the one with highest priority (lowest priority number)
    for(auto& mms : mmsGroup.mmsList) {
        for(auto& tray : mms.trayList) {
            if(!checkTrayIsReady(tray)) {
                continue;
            }
            
            PresetFilamentInfo matchedPreset = matchFilamentPreset(tray, mergedPresetMap);
            if(!matchedPreset.filamentName.empty()) {
                tray.filamentId = matchedPreset.filamentId;
                tray.settingId = matchedPreset.settingId;
                tray.filamentPresetName = matchedPreset.filamentName;
                tray.filamentPresetAlias = matchedPreset.filamentAlias;
                continue;
            }
        }
    }
    
    return;
}

// build preset filament map by standardized name
// loads vendor bundle and orca filament library, processes all filament types and returns merged map
std::map<std::string, std::vector<PrinterMmsManager::PresetFilamentInfo>> PrinterMmsManager::buildPresetFilamentMap(
    const PrinterNetworkInfo& printerNetworkInfo,
    const std::vector<double>& currentProjectNozzleDiameters)
{
    std::map<std::string, std::vector<PresetFilamentInfo>> mergedPresetMap;
    
    // load vendor bundle
    PresetBundle vendorBundle;
    try {
        vendorBundle.load_vendor_configs_from_json((boost::filesystem::path(Slic3r::resources_dir()) / "profiles").string(),
                                                   printerNetworkInfo.vendor, PresetBundle::LoadSystem,
                                                   ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr);                                        

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "PrinterMmsManager::buildPresetFilamentMap: get vendor configs failed: " << printerNetworkInfo.vendor << " " << e.what();
        return mergedPresetMap;
    }

    
    // load orca filament library
    PresetBundle orcaFilamentLibraryBundle;
    try {
        orcaFilamentLibraryBundle.load_vendor_configs_from_json((boost::filesystem::path(Slic3r::resources_dir()) / "profiles").string(),
                                                   PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                                   ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr);                                        

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "PrinterMmsManager::buildPresetFilamentMap: get orca filament library failed: " << e.what();
    }
    // build printer preset map from vendor bundle
    std::map<std::string, PrinterPresetInfo> printerPresetMap;
    for(const auto& printer : vendorBundle.printers) {
        if(!printer.is_system) continue;
        
        PrinterPresetInfo info;
        auto printerModelOpt = printer.config.option<ConfigOptionString>("printer_model");
        if(printerModelOpt) {
            info.printerModel = printerModelOpt->value;
        }
        
        auto nozzleDiameterOpt = printer.config.option<ConfigOptionFloats>("nozzle_diameter");
        if(nozzleDiameterOpt) {
            info.nozzleDiameters = nozzleDiameterOpt->values;
        }
        
        printerPresetMap[printer.name] = info;
    }

    // process vendor specific filaments (priority 1)
    processFilamentsFromBundle(vendorBundle, mergedPresetMap, printerNetworkInfo, printerNetworkInfo.vendor, printerPresetMap, currentProjectNozzleDiameters, 1, false, true);

    // process vendor generic filaments (priority 2)
    processFilamentsFromBundle(vendorBundle, mergedPresetMap, printerNetworkInfo, "GENERIC", printerPresetMap, currentProjectNozzleDiameters, 2, true, true);

    // process orca generic filaments (priority 3)
    processFilamentsFromBundle(orcaFilamentLibraryBundle, mergedPresetMap, printerNetworkInfo, "GENERIC", printerPresetMap, currentProjectNozzleDiameters, 3, true, false);
    
    return mergedPresetMap;
}

// process filaments from bundle and add to merged preset map
void PrinterMmsManager::processFilamentsFromBundle(
    const PresetBundle& bundle,
    std::map<std::string, std::vector<PresetFilamentInfo>>& mergedPresetMap,
    const PrinterNetworkInfo& printerNetworkInfo,
    const std::string& sourceVendor,
    const std::map<std::string, PrinterPresetInfo>& printerPresetMap,
    const std::vector<double>& currentProjectNozzleDiameters,
    int priority,
    bool isGeneric,
    bool checkCompatible)
{
    for (const auto& filament : bundle.filaments) {
        if (!filament.is_system) continue;
        
        auto* filament_type_opt = dynamic_cast<const ConfigOptionStrings*>(filament.config.option("filament_type"));
        if(!filament_type_opt || filament_type_opt->values.empty()) continue;

        if(checkCompatible && !isFilamentCompatible(filament, printerNetworkInfo, printerPresetMap, currentProjectNozzleDiameters)) continue;
        
        std::string name = boost::to_upper_copy(filament.name);
        bool isGenericFilament = (name.find("GENERIC") != std::string::npos);
        if(isGeneric != isGenericFilament) continue;
        
        PresetFilamentInfo info;
        info.filamentId = filament.filament_id;
        info.settingId = filament.setting_id;
        info.vendor = sourceVendor;
        info.filamentAlias = filament.alias;
        info.filamentName = filament.name;
        info.filamentType = filament_type_opt->values[0];
        info.priority = priority;
        
        std::string standardizedName = standardizeFilamentName(filament.alias, sourceVendor);
        mergedPresetMap[standardizedName].push_back(info);
    }
}

// check filament compatible
bool PrinterMmsManager::isFilamentCompatible(
    const Preset& filament,
    const PrinterNetworkInfo& printerNetworkInfo,
    const std::map<std::string, PrinterPresetInfo>& printerPresetMap,
    const std::vector<double>& currentProjectNozzleDiameters)
{
    const auto compatiblePrinters = filament.config.option<ConfigOptionStrings>("compatible_printers");
    if(!compatiblePrinters) return false;
    
    for (const std::string& printer_name : compatiblePrinters->values) {
        auto it = printerPresetMap.find(printer_name);
        if(it == printerPresetMap.end() || it->second.printerModel != printerNetworkInfo.printerModel) continue;
        
        if(currentProjectNozzleDiameters.empty()) return true;
        
        for(double currentProjectNozzleDiameter : currentProjectNozzleDiameters) {
            for(double presetNozzle : it->second.nozzleDiameters) {
                if(std::abs(currentProjectNozzleDiameter - presetNozzle) < 0.01) return true;
            }
        }
        continue;
    }
    return false;
}



PrinterNetworkResult<PrinterMmsGroup> PrinterMmsManager::getPrinterMmsInfo(const std::string& printerId)
{
    PrinterNetworkInfo printerNetworkInfo = PrinterManager::getInstance()->getPrinterNetworkInfo(printerId);
    if(printerNetworkInfo.printerId.empty()) {
        return PrinterNetworkResult<PrinterMmsGroup>(PrinterNetworkErrorCode::PRINTER_NOT_FOUND, PrinterMmsGroup());
    }
    PrinterNetworkResult<PrinterMmsGroup> mmsGroupResult = PrinterManager::getInstance()->getPrinterMmsInfo(printerId);
    if(!mmsGroupResult.isSuccess()) {
        return mmsGroupResult;
    }
    PrinterMmsGroup mmsGroup = mmsGroupResult.data.value();

    getMmsTrayFilamentId(printerNetworkInfo, mmsGroup);
    
    // match filament id in system preset to mms tray
    // set tray index
    std::vector<std::string> trayIndexList = {"A","B","C","D","E","F","G","H"};
    int mmsIndex = 0;
    for(auto& mms : mmsGroup.mmsList) {
        if(mmsIndex >= trayIndexList.size()) {
            BOOST_LOG_TRIVIAL(error) << "PrinterMmsManager::getPrinterMmsInfo: tray index list is not enough";
            break;
        }
        std::string mmsIndexStr =trayIndexList[mmsIndex];
        int trayIndex = 1;
        mmsIndex++;
        for(auto& tray : mms.trayList) {
            tray.trayName = mmsIndexStr + std::to_string(trayIndex);
            trayIndex++;        
        }
    }
    return PrinterNetworkResult<PrinterMmsGroup>(PrinterNetworkErrorCode::SUCCESS, mmsGroup);
}

bool PrinterMmsManager::checkTrayIsReady(const PrinterMmsTray& tray) {
    if(tray.status != TRAY_STATUS_LOADED && tray.status != TRAY_STATUS_PRELOADED) {
        return false;
    }
    if(tray.filamentType.empty() || tray.filamentName.empty() || tray.filamentColor.empty()) {
        return false;
    }
    return true;
}

// find best match by standardized name, fallback to name equals type
PrinterMmsManager::PresetFilamentInfo PrinterMmsManager::matchFilamentPreset(
    const PrinterMmsTray& tray,
    const std::map<std::string, std::vector<PresetFilamentInfo>>& presetMap)
{
    const std::string& filamentName = tray.filamentName;
    const std::string& filamentType = tray.filamentType;
    const std::string& trayVendor   = tray.vendor;

    if(filamentName.empty()) {
        return PresetFilamentInfo();
    }
    
    std::string standardizedName = standardizeFilamentName(filamentName, trayVendor);
    std::string standardizedType = "";
    if(!filamentType.empty()) {
        standardizedType = boost::to_upper_copy(filamentType);
        boost::trim(standardizedType);
    }
    std::string vendorUpper = boost::to_upper_copy(trayVendor);
    boost::trim(vendorUpper);
    auto isVendorMatchedPreset = [&vendorUpper](const PresetFilamentInfo& preset) -> bool {
        if(vendorUpper.empty()) {
            return false;
        }
        std::string presetVendor = boost::to_upper_copy(preset.vendor);
        boost::trim(presetVendor);
        return presetVendor == vendorUpper;
    };
    auto isBetterWithVendorPriority = [&isVendorMatchedPreset](const PresetFilamentInfo& candidate, const PresetFilamentInfo& current) -> bool {
        bool candidateVendorMatched = isVendorMatchedPreset(candidate);
        bool currentVendorMatched = isVendorMatchedPreset(current);
        if(candidateVendorMatched != currentVendorMatched) {
            return candidateVendorMatched;
        }
        return candidate.priority < current.priority;
    };
    
    auto it = presetMap.find(standardizedName);
    if(it != presetMap.end() && !it->second.empty()) {
        // first pass: choose best candidate whose vendor matches tray vendor
        PresetFilamentInfo matchedPreset;
        matchedPreset.priority = std::numeric_limits<int>::max();
        bool foundVendorMatched = false;
        for(const auto& candidate : it->second) {
            if(isVendorMatchedPreset(candidate) && candidate.priority < matchedPreset.priority) {
                matchedPreset = candidate;
                foundVendorMatched = true;
            }
        }
        if(foundVendorMatched) {
            return matchedPreset;
        }

        // second pass: fallback to lowest priority among all candidates
        for(const auto& candidate : it->second) {
            if(candidate.priority < matchedPreset.priority) {
                matchedPreset = candidate;
            }
        }
        return matchedPreset;
    }
    
    // fallback: match by name equals type if name matching failed
    if(standardizedType.empty()) {
        return PresetFilamentInfo();
    }
    
    PresetFilamentInfo bestMatch;
    bestMatch.priority = std::numeric_limits<int>::max();
    PresetFilamentInfo fallbackMatch;
    fallbackMatch.priority = std::numeric_limits<int>::max();
    
    for(const auto& entry : presetMap) {
        for(const auto& preset : entry.second) {
            std::string presetStandardizedName = standardizeFilamentName(preset.filamentAlias, trayVendor);
            std::string presetType = boost::to_upper_copy(preset.filamentType);
            boost::trim(presetType);
            
            // match by name equals type
            if(standardizedType == presetStandardizedName || standardizedName == presetType) {
                if(isBetterWithVendorPriority(preset, bestMatch)) {
                    bestMatch = preset;
                }
                continue;
            }
            
            // fallback: match by same filament type
            if(presetType == standardizedType && isBetterWithVendorPriority(preset, fallbackMatch)) {
                fallbackMatch = preset;
            }
        }
    }
    return bestMatch.filamentName.empty() ? fallbackMatch : bestMatch;
}

void PrinterMmsManager::getFilamentMmsMapping(std::vector<PrintFilamentMmsMapping>& printFilamentMmsMapping, const PrinterMmsGroup& mmsGroup)
{
    // Load mapping from JSON file
    nlohmann::json mappingJson = loadFilamentMmsMappingFromFile();
    
    for (auto& printFilament : printFilamentMmsMapping) {
        // A material override is a decision about one send, not a standing permission: it is
        // never saved or restored, so a mismatched tray always asks again.
        printFilament.materialOverride = false;
        StandardColor standardColor = getStandardColor(printFilament.filamentColor);
        std::string filamentStandardColor = standardColor.colorHex;
        std::string StandardColorName = standardColor.colorName;
        if(StandardColorName.empty() || filamentStandardColor.empty()) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": standard color name or filament standard color is empty, filamentType: %s, filamentAlias: %s, filamentColor: %s")
                                                      % printFilament.filamentType % printFilament.filamentAlias % printFilament.filamentColor;
            continue;
        }
        std::string mmsMappingFilamentType = "";
        std::string mmsMappingFilamentName = "";
        std::string mmsMappingFilamentColor = "";
        bool isMapped = false;
        
        // Check if mapping exists in JSON
        if (mappingJson.contains(printFilament.filamentType) &&
            mappingJson[printFilament.filamentType].contains(printFilament.filamentAlias) &&
            mappingJson[printFilament.filamentType][printFilament.filamentAlias].contains(filamentStandardColor)) {
            
            auto mapping = mappingJson[printFilament.filamentType][printFilament.filamentAlias][filamentStandardColor];
            mmsMappingFilamentType = mapping.value("mappedFilamentType", "");
            mmsMappingFilamentName = mapping.value("mappedFilamentName", "");
            mmsMappingFilamentColor = mapping.value("mappedFilamentColor", "");
        }
        if (!mmsMappingFilamentType.empty() && !mmsMappingFilamentName.empty() && !mmsMappingFilamentColor.empty()) {
            for (auto& mms : mmsGroup.mmsList) {
                for (auto& tray : mms.trayList) {
                    if(!checkTrayIsReady(tray)) {
                        continue;
                    }
                    if (boost::to_upper_copy(tray.filamentType) == boost::to_upper_copy(mmsMappingFilamentType) &&
                        boost::to_upper_copy(tray.filamentName) == boost::to_upper_copy(mmsMappingFilamentName) &&
                        boost::to_upper_copy(tray.filamentColor) == boost::to_upper_copy(mmsMappingFilamentColor)) {
                        printFilament.mappedMmsFilament.trayId           = tray.trayId;
                        printFilament.mappedMmsFilament.mmsId            = tray.mmsId;
                        printFilament.mappedMmsFilament.trayName         = tray.trayName;
                        printFilament.mappedMmsFilament.filamentType     = tray.filamentType;
                        printFilament.mappedMmsFilament.filamentName     = tray.filamentName;
                        printFilament.mappedMmsFilament.filamentColor    = tray.filamentColor;
                        printFilament.mappedMmsFilament.filamentDiameter = tray.filamentDiameter;
                        printFilament.mappedMmsFilament.minNozzleTemp    = tray.minNozzleTemp;
                        printFilament.mappedMmsFilament.maxNozzleTemp    = tray.maxNozzleTemp;
                        printFilament.mappedMmsFilament.minBedTemp       = tray.minBedTemp;
                        printFilament.mappedMmsFilament.maxBedTemp       = tray.maxBedTemp;
                        printFilament.mappedMmsFilament.status           = tray.status;
                        isMapped                                         = true;
                        break;
                    }
                }
            }
        }

        if (isMapped) {
            continue;
        }
        // not mapped or mapped filament not exist
        // build filament preset map from print filament
        std::map<std::string, std::vector<PresetFilamentInfo>> filamentPresetMap;
        PresetFilamentInfo filamentInfo;
        filamentInfo.filamentId = printFilament.filamentId;
        filamentInfo.settingId = printFilament.settingId;
        filamentInfo.vendor = printFilament.vendor;
        filamentInfo.filamentAlias = printFilament.filamentAlias;
        filamentInfo.filamentName = printFilament.filamentName;
        filamentInfo.filamentType = printFilament.filamentType;
        filamentInfo.priority = 1; // default priority for user filament
        std::string standardizedName = standardizeFilamentName(printFilament.filamentAlias, printFilament.vendor);
        filamentPresetMap[standardizedName].push_back(filamentInfo);

        PrinterMmsTray mappedTray;
        for (auto& mms : mmsGroup.mmsList) {
            for (auto& tray : mms.trayList) {
                if(!checkTrayIsReady(tray)) {
                    continue;
                }
                // check if the filament color is the same as the standard color
                if(boost::to_upper_copy(getStandardColor(tray.filamentColor).colorHex) != boost::to_upper_copy(filamentStandardColor)){
                    continue;
                }
                mappedTray = tray;
                PresetFilamentInfo matchedPreset = matchFilamentPreset(mappedTray, filamentPresetMap);
                if(!matchedPreset.filamentName.empty()) {
                    mappedTray.filamentId = matchedPreset.filamentId;
                    mappedTray.settingId = matchedPreset.settingId;
                    mappedTray.filamentPresetName = matchedPreset.filamentName;
                    mappedTray.filamentPresetAlias = matchedPreset.filamentAlias;
                    isMapped = true;
                    break;
                }
            }
            if(isMapped) break;
        }
        if(isMapped) {
            printFilament.mappedMmsFilament.trayName         = mappedTray.trayName;
            printFilament.mappedMmsFilament.trayId           = mappedTray.trayId;
            printFilament.mappedMmsFilament.mmsId            = mappedTray.mmsId;
            printFilament.mappedMmsFilament.filamentName     = mappedTray.filamentName;
            printFilament.mappedMmsFilament.filamentColor    = mappedTray.filamentColor;
            printFilament.mappedMmsFilament.filamentType     = mappedTray.filamentType;
            printFilament.mappedMmsFilament.filamentDiameter = mappedTray.filamentDiameter;
            printFilament.mappedMmsFilament.minNozzleTemp    = mappedTray.minNozzleTemp;
            printFilament.mappedMmsFilament.maxNozzleTemp    = mappedTray.maxNozzleTemp;
            printFilament.mappedMmsFilament.minBedTemp       = mappedTray.minBedTemp;
            printFilament.mappedMmsFilament.maxBedTemp       = mappedTray.maxBedTemp;
            printFilament.mappedMmsFilament.status           = mappedTray.status;
        }
    }
}

void PrinterMmsManager::saveFilamentMmsMapping(std::vector<PrintFilamentMmsMapping>& printFilamentMmsMapping)
{
    // Load existing mappings first
    nlohmann::json mappingJson = loadFilamentMmsMappingFromFile();
    
    for (auto& printFilament : printFilamentMmsMapping) {
        if (printFilament.mappedMmsFilament.trayName.empty() || 
            printFilament.mappedMmsFilament.trayId.empty() || 
            printFilament.mappedMmsFilament.filamentName.empty() ||
            printFilament.mappedMmsFilament.filamentColor.empty()) {
            continue;
        }
        
        StandardColor mappedStandardColor = getStandardColor(printFilament.mappedMmsFilament.filamentColor);
        std::string mappedFilamentStandardColor = mappedStandardColor.colorHex;
        std::string mappedStandardColorName = mappedStandardColor.colorName;
        
        if(mappedStandardColorName.empty() || mappedFilamentStandardColor.empty()) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": color name or filament standard color is empty, filamentType: %s, filamentAlias: %s, filamentColor: %s")
                                                  % printFilament.filamentType % printFilament.filamentAlias % printFilament.filamentColor;
            continue;
        }
        StandardColor standardColor = getStandardColor(printFilament.filamentColor);
        std::string filamentStandardColor = standardColor.colorHex;
        std::string filamentStandardColorName = standardColor.colorName;
        
        if(filamentStandardColorName.empty() || filamentStandardColor.empty()) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": color name or filament standard color is empty, filamentType: %s, filamentAlias: %s, filamentColor: %s")
                                                  % printFilament.filamentType % printFilament.filamentAlias % printFilament.filamentColor;
            continue;
        }
        // Create three-level structure: filamentType -> filamentAlias -> filamentStandardColor
        if (!mappingJson.contains(printFilament.filamentType)) {
            mappingJson[printFilament.filamentType] = nlohmann::json::object();
        }
        if (!mappingJson[printFilament.filamentType].contains(printFilament.filamentAlias)) {
            mappingJson[printFilament.filamentType][printFilament.filamentAlias] = nlohmann::json::object();
        }

        // save mapping information, mapped filament info, print filament color converted to standard color 
        // when model multi-color editing, the same standard color will have many color values, unified converted to standard color
        // mapped filament color is the mms filament actual color, but also store the mms filament color corresponding to the standard color
        mappingJson[printFilament.filamentType][printFilament.filamentAlias][filamentStandardColor] = {
            {"mappedFilamentType", printFilament.mappedMmsFilament.filamentType},
            {"mappedFilamentName", printFilament.mappedMmsFilament.filamentName},
            {"mappedFilamentColor", printFilament.mappedMmsFilament.filamentColor},
            {"mappedFilamentStandardColor", mappedFilamentStandardColor},
            {"mappedFilamentStandardColorName", mappedStandardColorName},
            {"filamentStandardColor", filamentStandardColor},
            {"filamentStandardColorName", filamentStandardColorName}
        };
    }
    
    // Save merged mappings to JSON file
    saveFilamentMmsMappingToFile(mappingJson);
}

nlohmann::json PrinterMmsManager::loadFilamentMmsMappingFromFile()
{
    std::lock_guard<std::mutex> lock(mFilamentMmsMappingMutex);
    try {
        
        std::string filePath = (boost::filesystem::path(Slic3r::data_dir()) / "user" / "filament_mms_mapping.json").string();
        
        if (boost::filesystem::exists(filePath)) {
            boost::nowide::ifstream file(filePath);
            if (file.is_open()) {
                nlohmann::json jsonData;
                file >> jsonData;
                file.close();
                return jsonData;
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": failed to load filament MMS mapping file: %s") % e.what();
    }
    
    return nlohmann::json::object();
}

void PrinterMmsManager::saveFilamentMmsMappingToFile(const nlohmann::json& mappingJson)
{
    std::lock_guard<std::mutex> lock(mFilamentMmsMappingMutex);
    try {
        std::string filePath = (boost::filesystem::path(Slic3r::data_dir()) / "user" / "filament_mms_mapping.json").string();
        
        // Ensure directory exists
        boost::filesystem::path dir = boost::filesystem::path(filePath).parent_path();
        if (!boost::filesystem::exists(dir)) {
            boost::filesystem::create_directories(dir);
        }
        
        boost::nowide::ofstream file(filePath);
        if (file.is_open()) {
            file << mappingJson.dump(4); // Pretty print with 4 spaces indentation
            file.close();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": failed to save filament MMS mapping file: %s") % e.what();
    }
}

void PrinterMmsManager::removeFilamentMmsMapping(const std::string& filamentType, const std::string& filamentAlias, const std::string& filamentColor)
{
    std::lock_guard<std::mutex> lock(mFilamentMmsMappingMutex);
    try {
        // Load existing mappings
        nlohmann::json mappingJson = loadFilamentMmsMappingFromFile();
        
        std::string filamentStandardColor = getStandardColor(filamentColor).colorHex;
        
        // Remove specific mapping if it exists
        if (mappingJson.contains(filamentType) &&
            mappingJson[filamentType].contains(filamentAlias) &&
            mappingJson[filamentType][filamentAlias].contains(filamentStandardColor)) {
            
            mappingJson[filamentType][filamentAlias].erase(filamentStandardColor);
            
            // Clean up empty objects
            if (mappingJson[filamentType][filamentAlias].empty()) {
                mappingJson[filamentType].erase(filamentAlias);
            }
            if (mappingJson[filamentType].empty()) {
                mappingJson.erase(filamentType);
            }
            
            // Save updated mappings
            saveFilamentMmsMappingToFile(mappingJson);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": failed to remove filament MMS mapping: %s") % e.what();
    }
}

} // namespace Slic3r

