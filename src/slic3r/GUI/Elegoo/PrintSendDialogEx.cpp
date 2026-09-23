#include "PrintSendDialogEx.hpp"
#include "slic3r/GUI/ConfigWizard.hpp"
#include <string.h>
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r_version.h"
#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>
#include "slic3r/GUI/MainFrame.hpp"
#include <boost/dll.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/Utils/Http.hpp>
#include <libslic3r/miniz_extension.hpp>
#include <libslic3r/Utils.hpp>
#include <wx/wx.h>
#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>
#include <wx/mstream.h>
#include <wx/base64.h>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <thread>
#include <memory>
#include "slic3r/Utils/Elegoo/PrinterManager.hpp"
#include "slic3r/Utils/Elegoo/PrinterMmsManager.hpp"
#include <slic3r/Utils/WebviewIPCManager.h>
#include <algorithm>
#include <cctype>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

#define HAS_MMS_HEIGHT 800
#define NO_MMS_HEIGHT 650

using namespace nlohmann;

namespace Slic3r { namespace GUI {

namespace {

// Drops a reinforcement/grade suffix so a PETG slice accepts a PETG-CF tray: the preset
// reports PETG, the tray reports PETG-CF. Keep in sync with printsend.js.
std::string standardizeFilamentType(const std::string& type)
{
    std::string t = boost::to_upper_copy(type);
    boost::trim(t);
    if (t.empty()) {
        return t;
    }
    if (t.back() == '+') { // PLA+ -> PLA
        t.pop_back();
        boost::trim(t);
    }
    const size_t dash = t.rfind('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= t.size()) {
        return t;
    }
    const std::string suffix = t.substr(dash + 1);

    // "-S" is not folded: get_filament_type() adds it to mark support material (PLA-S),
    // so folding it would match a support slice to a model-material tray.
    const bool isReinforcement = suffix == "CF" || suffix == "GF" || suffix == "AERO";

    // Grades: CF25 / GF30 (fibre content) and 95A / 64D (durometer).
    bool isGrade = false;
    if ((suffix.size() > 2) && (suffix.compare(0, 2, "CF") == 0 || suffix.compare(0, 2, "GF") == 0)) {
        isGrade = std::all_of(suffix.begin() + 2, suffix.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    } else if (suffix.size() > 1 && (suffix.back() == 'A' || suffix.back() == 'D')) {
        isGrade = std::all_of(suffix.begin(), suffix.end() - 1, [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    if (isReinforcement || isGrade) {
        return t.substr(0, dash);
    }
    return t;
}

// mmsId/trayId are slot positions, not spool identities, so they survive a spool change.
const PrinterMmsTray* findLiveTray(const PrinterMmsGroup& group, const PrinterMmsTray& pick)
{
    for (const auto& mms : group.mmsList) {
        for (const auto& tray : mms.trayList) {
            if (tray.mmsId == pick.mmsId && tray.trayId == pick.trayId) {
                return &tray;
            }
        }
    }
    return nullptr;
}

// The slot map carries indices only, so a swapped spool is undetectable downstream.
bool liveTrayMatchesPick(const PrinterMmsTray& live, const PrinterMmsTray& pick)
{
    return boost::iequals(live.filamentType, pick.filamentType) &&
           boost::iequals(live.filamentName, pick.filamentName) &&
           boost::iequals(live.filamentColor, pick.filamentColor);
}

// A tray reporting no type stays usable: unknown is not a proven mismatch.
bool trayMaterialMatches(const std::string& printFilamentType, const std::string& trayFilamentType)
{
    const std::string trayType = standardizeFilamentType(trayFilamentType);
    if (trayType.empty()) {
        return true;
    }
    return standardizeFilamentType(printFilamentType) == trayType;
}

} // namespace

PrintSendDialogEx::PrintSendDialogEx(Plater* plater, int printPlateIdx, const boost::filesystem::path& path)
    :  DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Send G-code to printer host"))
    , mPlater(plater)
    , mPrintPlateIdx(printPlateIdx)
    , mTimeLapse(0)
    , mHeatedBedLeveling(0)
    , mBedType(BedType::btPTE)
    , mPostUploadAction(PrintHostPostUploadAction::None)
    , mSwitchToDeviceTab(false)
    , mPath(path)
{
    // Bind close event to handle async operations
    Bind(wxEVT_CLOSE_WINDOW, &PrintSendDialogEx::OnCloseWindow, this);

        // Bind ESC key hook to disable ESC key closing the dialog
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            // Do nothing - disable ESC key closing the dialog
            return;
        }
        e.Skip();
    });
}

PrintSendDialogEx::~PrintSendDialogEx()
{
    // First: joining the pool here keeps handlers off the members destroyed below.
    mIpc.reset();
    // anything already queued onto the GUI thread now sees an expired token
    mAlive.reset();
}

void PrintSendDialogEx::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    Refresh();
}

void PrintSendDialogEx::init()
{
    const AppConfig* app_config = wxGetApp().app_config;

    auto preset_bundle = wxGetApp().preset_bundle;
    auto model_id      = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);

    SetIcon(wxNullIcon);
    // DestroyChildren();
    mBrowser = WebView::CreateWebView(this, "");
    if (mBrowser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": could not init m_browser";
        return;
    }

    mIpc = std::make_unique<webviewIpc::WebviewIPCManager>(mBrowser);
    setupIPCHandlers();
    // mBrowser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrintSendDialogEx::onScriptMessage, this);
    mBrowser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    wxString TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/printer/print_send/index.html").make_preferred().string());
    TargetUrl = "file://" + TargetUrl;
    wxString strlang = wxGetApp().current_language_code_safe();
    if (strlang != "")
        TargetUrl = wxString::Format("%s?lang=%s", TargetUrl, strlang);
    if (wxGetApp().app_config->get_bool("developer_mode")) {
        TargetUrl = TargetUrl + "&dev=true";
    }
    mBrowser->LoadURL(TargetUrl);

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(topsizer);
    topsizer->Add(mBrowser, wxSizerFlags().Expand().Proportion(1));
    wxSize pSize = FromDIP(wxSize(860, HAS_MMS_HEIGHT));
    SetSize(pSize);
    CenterOnParent();

    std::string uploadAndPrint = app_config->get("recent", CONFIG_KEY_UPLOADANDPRINT);
    if (!uploadAndPrint.empty())
        mPostUploadAction = static_cast<PrintHostPostUploadAction>(std::stoi(uploadAndPrint));
    std::string timeLapse = app_config->get("recent", CONFIG_KEY_TIMELAPSE);
    if (!timeLapse.empty())
        mTimeLapse = std::stoi(timeLapse);
    std::string heatedBedLeveling = app_config->get("recent", CONFIG_KEY_HEATEDBEDLEVELING);
    if (!heatedBedLeveling.empty())
        mHeatedBedLeveling = std::stoi(heatedBedLeveling);
    std::string bedType = app_config->get("recent", CONFIG_KEY_BEDTYPE);
    if (!bedType.empty())
        mBedType = static_cast<BedType>(std::stoi(bedType));

    std::string autoRefill = app_config->get("recent", CONFIG_KEY_AUTO_REFILL);
    if (!autoRefill.empty())
        mAutoRefill = std::stoi(autoRefill);

    std::string switchToDeviceTab = app_config->get("recent", CONFIG_KEY_SWITCH_TO_DEVICE_TAB);
    if (!switchToDeviceTab.empty())
        mSwitchToDeviceTab = std::stoi(switchToDeviceTab);

    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/') {
        recent_path += '/';
    }
    recent_path += mPath.filename().wstring();

    // if (logo) {
    //     logo->Hide();
    // }
    // //hide content_sizer child
    // if(content_sizer){
    //     auto child = content_sizer->GetChildren();
    //     for (auto& item : child) {
    //         if(item->IsWindow() && item->GetWindow()!=nullptr) {
    //             item->GetWindow()->Hide();
    //         }
    //     }
    // }

    // Cache the model name for use in preparePrintTask
    mModelName = recent_path;
    if (mModelName.size() >= 6 && mModelName.compare(mModelName.size() - 6, 6, ".gcode") == 0)
        mModelName = mModelName.substr(0, mModelName.size() - 6);

    // if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
    //     m_valid_suffix = recent_path.substr(extension_start);
    // mProjectName = getCurrentProjectName();
}

std::string PrintSendDialogEx::getCurrentProjectName()
{
    wxString filename = mPlater->get_export_gcode_filename("", true, mPrintPlateIdx == PLATE_ALL_IDX ? true : false);
    if (mPrintPlateIdx == PLATE_ALL_IDX && filename.empty()) {
        filename = _L("Untitled");
    }

    if (filename.empty()) {
        filename = mPlater->get_export_gcode_filename("", true);
        if (filename.empty())
            filename = _L("Untitled");
    }

    fs::path    filenamePath(filename.c_str());
    std::string projectName = filenamePath.filename().string();
    if (from_u8(projectName).find(_L("Untitled")) != wxString::npos) {
        PartPlate* partPlate = mPlater->get_partplate_list().get_plate(mPrintPlateIdx);
        if (partPlate) {
            if (std::vector<ModelObject*> objects = partPlate->get_objects_on_this_plate(); objects.size() > 0) {
                projectName = objects[0]->name;
                for (int i = 1; i < objects.size(); i++) {
                    projectName += (" + " + objects[i]->name);
                }
            }
            if (projectName.size() > 100) {
                projectName = projectName.substr(0, 97) + "...";
            }
        }
    }
    const std::string invalidChars = "<>[]:\\/|?*\"";
    projectName.erase(std::remove_if(projectName.begin(), projectName.end(),
                                     [&invalidChars](char c) { return invalidChars.find(c) != std::string::npos; }),
                      projectName.end());
    return projectName;
}

void PrintSendDialogEx::setupIPCHandlers()
{
    if (!mIpc)
        return;

    // Handle request_print_task (async due to time-consuming preparePrintTask operation)
    mIpc->onRequestAsync("request_print_task", [this](const IPCRequest&                     request,
                                                      std::function<void(const IPCResult&)> sendResponse) {
        std::string printerId = request.params.value("printerId", "");

            try {
                IPCResult response = this->preparePrintTask(printerId);
                sendResponse(response);
                    
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_print_task: %s") % e.what();
                sendResponse(IPCResult::error(std::string("Print task preparation failed: ") + e.what()));
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unknown error in request_print_task";
                sendResponse(IPCResult::error("Print task preparation failed: Unknown error"));
            }
    });

    // Handle request_printer_list
    mIpc->onRequest("request_printer_list", [this](const IPCRequest& request) {
        try {
            return getPrinterList();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_printer_list: %s") % e.what();
            return IPCResult::error("Failed to get printer list");
        }
    });

    // Handle cancel_print
    mIpc->onEvent("cancel_print", [this](const IPCEvent& event) {
        try {
            callAfterIfAlive([this]() { onCancel(); });
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Error in cancel_print: " << e.what();
        }
    });

    mIpc->onRequestAsync("start_upload", [this](const IPCRequest& request,
                                                 std::function<void(const IPCResult&)> sendResponse) {
        try {
            auto result = onPrint(request.params);
            if (result.code == 0) {
                // onPrint is slow enough that the dialog can close before it returns
                callAfterIfAlive([this]() { EndModal(wxID_OK); });
            } else {
                sendResponse(result);
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Error in start_upload: " << e.what();
            sendResponse(IPCResult::error(std::string("Upload failed: ") + e.what()));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Unknown error in start_upload";
            sendResponse(IPCResult::error("Upload failed: Unknown error"));
        }
    });

    mIpc->onEvent("expand_window", [this](const IPCEvent& event) {
        try {
            bool expand = event.data.value("expand", false);
            callAfterIfAlive([this, expand]() {
                SetSize(FromDIP(wxSize(860, expand ? HAS_MMS_HEIGHT : NO_MMS_HEIGHT)));
            });
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Error in expand_window: " << e.what();
        }
    });

    mIpc->onRequest("get_current_bed_type", [this](const IPCRequest& request) {
        try {
            int         bedType    = getCurrentBedType();
            std::string bedTypeStr = "";
            if (bedType == BedType::btPC)
                bedTypeStr = "btPC"; // B
            else if (bedType == BedType::btPTE)
                bedTypeStr = "btPTE"; // A
            else {
                bedTypeStr = "unknown";
            }

            nlohmann::json response = nlohmann::json::object();
            response["bedType"]     = bedTypeStr;
            return IPCResult::success(response);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_printer_list: %s") % e.what();
            return IPCResult::error("Failed to get printer list");
        }
    });

    // Tray state for one additional printer; async because it talks to that printer.
    mIpc->onRequestAsync("request_additional_mms_info", [this](const IPCRequest&                     request,
                                                               std::function<void(const IPCResult&)> sendResponse) {
        std::string printerId = request.params.value("printerId", "");
        try {
            sendResponse(this->getAdditionalPrinterMmsInfo(printerId));
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_additional_mms_info: %s") % e.what();
            sendResponse(IPCResult::error(std::string("MMS info request failed: ") + e.what()));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unknown error in request_additional_mms_info";
            sendResponse(IPCResult::error("MMS info request failed: Unknown error"));
        }
    });

    // Handle request_mms_info (async due to potentially time-consuming getPrinterMmsInfo operation)
    mIpc->onRequestAsync("request_mms_info", [this](const IPCRequest& request,
                                                     std::function<void(const IPCResult&)> sendResponse) {
        std::string printerId = request.params.value("printerId", "");
        try {
            IPCResult response = this->getPrinterMmsInfo(printerId);
            sendResponse(response);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_mms_info: %s") % e.what();
            sendResponse(IPCResult::error(std::string("MMS info request failed: ") + e.what()));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unknown error in request_mms_info";
            sendResponse(IPCResult::error("MMS info request failed: Unknown error"));
        }
    });
}
IPCResult PrintSendDialogEx::preparePrintTask(const std::string& printerId)
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    nlohmann::json printTask     = json::object();
    auto           preset_bundle = wxGetApp().preset_bundle;

    const Print&   print     = mPlater->get_partplate_list().get_current_fff_print();
    const auto&    stats     = print.print_statistics();
    nlohmann::json printInfo = json::object();
    printInfo["printTime"]   = stats.estimated_normal_print_time;
    printInfo["totalWeight"] = stats.total_weight;

    int layerCount = 0;
    for (const PrintObject* object : print.objects()) {
        layerCount = std::max(layerCount, (int) object->layer_count());
    }
    printInfo["layerCount"] = layerCount;

    // Use cached model name instead of getting it from UI control
    std::string modelName = mModelName.ToUTF8().data();

    printInfo["modelName"]         = modelName;
    printInfo["timeLapse"]         = mTimeLapse == 1;
    printInfo["heatedBedLeveling"] = mHeatedBedLeveling == 1;
    printInfo["switchToDeviceTab"] = mSwitchToDeviceTab;
    printInfo["uploadAndPrint"]    = mPostUploadAction == PrintHostPostUploadAction::StartPrint;
    printInfo["autoRefill"]        = mAutoRefill == 1;
    if (mBedType == BedType::btPC)
        printInfo["bedType"] = "btPC"; // B
    else
        printInfo["bedType"] = "btPTE"; // A

    ThumbnailData& data = mPlater->get_partplate_list().get_curr_plate()->thumbnail_data;
    wxImage        image;
    if (data.is_valid()) {
        image = wxImage(data.width, data.height);
        image.InitAlpha();
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char* px = (unsigned char*) data.pixels.data() + 4 * (rr + c);
                image.SetRGB((int) c, (int) r, px[0], px[1], px[2]);
                image.SetAlpha((int) c, (int) r, px[3]);
            }
        }
        image = image.Rescale(FromDIP(256), FromDIP(256));
        wxMemoryOutputStream mem;
        image.SaveFile(mem, wxBITMAP_TYPE_PNG);
        size_t         len = mem.GetSize();
        wxMemoryBuffer buffer(len);
        mem.CopyTo(buffer.GetData(), len);
        printInfo["thumbnail"] = wxBase64Encode(buffer.GetData(), len).ToStdString();
    }

    std::map<std::string, Preset*> nameToPreset;
    for (int i = 0; i < preset_bundle->filaments.size(); ++i) {
        Preset* preset             = &preset_bundle->filaments.preset(i);
        nameToPreset[preset->name] = preset;
    }

    mMmsGroup = PrinterMmsGroup();
    // get printfilament list
    std::vector<PrintFilamentMmsMapping> projectFilamentList;
    mPrintFilamentList.clear();
    for (const auto& filamentName : preset_bundle->filament_presets) {
        auto it = nameToPreset.find(filamentName);
        if (it != nameToPreset.end()) {
            PrintFilamentMmsMapping filament;
            Preset*                 preset        = it->second;
            std::string             filamentAlias = preset->alias;
            std::string             displayedFilamentType;
            std::string             filamentType = preset->config.get_filament_type(displayedFilamentType);

            // get filament density to calculate filament weight
            float                     density            = 0.0;
            const ConfigOptionFloats* filament_densities = preset->config.option<ConfigOptionFloats>("filament_density");
            if (filament_densities != nullptr) {
                density = filament_densities->values[0];
            }

            filament.filamentType = filamentType;
            filament.filamentId   = preset->filament_id;
            filament.settingId    = preset->setting_id;
            const ConfigOptionStrings* filament_vendors = preset->config.option<ConfigOptionStrings>("filament_vendor");
            filament.vendor = (filament_vendors != nullptr && !filament_vendors->values.empty()) ? filament_vendors->values[0] : "";
            filament.filamentName = filamentName;
            // alias and filamentType is used to match filament in mms
            filament.filamentAlias   = filamentAlias;
            filament.filamentWeight  = 0;
            filament.filamentDensity = density;
            projectFilamentList.push_back(filament);
        }
    }
    // calculate filament weight
    auto extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_filaments();
    for (auto i = 0; i < extruders.size(); i++) {
        int extruderIdx = extruders[i] - 1;
        if (extruderIdx < 0 || extruderIdx >= (int) projectFilamentList.size())
            continue;
        auto info          = projectFilamentList[extruderIdx];
        info.index         = extruderIdx;
        auto colour        = wxGetApp().preset_bundle->project_config.opt_string("filament_colour", (unsigned int) extruderIdx);
        info.filamentColor = colour;
        float total_weight = 0.0;

        const auto& stats = print.print_statistics();

        double model_volume_mm3 = 0.0;
        auto   model_it         = stats.filament_stats.find(extruderIdx);
        if (model_it != stats.filament_stats.end()) {
            model_volume_mm3 = model_it->second;
        }

        double wipe_tower_volume_mm3 = 0.0;
        double support_volume_mm3    = 0.0;
        double flush_per_filament    = 0.0;

        auto current_plate = mPlater->get_partplate_list().get_curr_plate();
        if (current_plate && current_plate->get_slice_result()) {
            const auto& gcode_stats = current_plate->get_slice_result()->print_statistics;

            auto wipe_tower_it = gcode_stats.wipe_tower_volumes_per_extruder.find(extruderIdx);
            if (wipe_tower_it != gcode_stats.wipe_tower_volumes_per_extruder.end()) {
                wipe_tower_volume_mm3 = wipe_tower_it->second;
            }

            auto support_it = gcode_stats.support_volumes_per_extruder.find(extruderIdx);
            if (support_it != gcode_stats.support_volumes_per_extruder.end()) {
                support_volume_mm3 = support_it->second;
            }

            auto flush_per_filament_it = gcode_stats.flush_per_filament.find(extruderIdx);
            if (flush_per_filament_it != gcode_stats.flush_per_filament.end()) {
                flush_per_filament = flush_per_filament_it->second;
            }
        }
        double total_volume_mm3 = model_volume_mm3 + wipe_tower_volume_mm3 + support_volume_mm3 + flush_per_filament;

        if (total_volume_mm3 > 0) {
            double raw_weight = total_volume_mm3 * info.filamentDensity * 0.001;

            if (raw_weight > 0) {
                double magnitude  = pow(10, floor(log10(raw_weight)));
                double normalized = raw_weight / magnitude;
                total_weight      = round(normalized * 100) / 100 * magnitude;
            } else {
                total_weight = 0.0;
            }
        }
        info.filamentWeight = total_weight;
        mPrintFilamentList.push_back(info);
    }

    auto        cfg               = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    std::string printerModel      = "";
    auto        printerModelValue = cfg.option<ConfigOptionString>("printer_model");
    if (printerModelValue) {
        printerModel = printerModelValue->value;
    }
    printInfo["currentProjectPrinterModel"] = printerModel;
    mHasMms = false;
    nlohmann::json filamentList = json::array();
    for (auto& filament : mPrintFilamentList) {
        filamentList.push_back(convertPrintFilamentMmsMappingToJson(filament));
    }

    printInfo["filamentList"] = filamentList;

    IPCResult result;
    result.data    = printInfo;
    result.code    = 0;
    result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
    return result;
}

IPCResult PrintSendDialogEx::getPrinterMmsInfo(const std::string &printerId)
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    IPCResult result;
    mMmsGroup = PrinterMmsGroup();
    PrinterNetworkInfo printerNetworkInfo = PrinterManager::getInstance()->getPrinterNetworkInfo(printerId);
    if(!printerNetworkInfo.systemCapabilities.supportsMultiFilament) {
        result.data = json::object();
        result.data["mmsInfo"] = json::object();
        result.data["mappedFilamentList"] = json::array();
        result.code = static_cast<int>(PrinterNetworkErrorCode::SUCCESS);
        result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
        return result;
    }
    PrinterNetworkResult<PrinterMmsGroup> res = PrinterMmsManager::getInstance()->getPrinterMmsInfo(printerId);
    if (res.isSuccess()) {
        mMmsGroup = res.data.value();
    } else {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": failed to get printer mms info";
        return result;
    }
    nlohmann::json mmsInfo = convertPrinterMmsGroupToJson(mMmsGroup);
    if(mMmsGroup.mmsList.size() == 0 || !mMmsGroup.connected) {
        mHasMms = false;
    } else {
        mHasMms = true;
    }
    
    PrinterMmsManager::getInstance()->getFilamentMmsMapping(mPrintFilamentList, mMmsGroup);
    nlohmann::json filamentList = json::array();

    result.data = json::object();
    result.data["mmsInfo"] = mmsInfo;
    for(auto& filament : mPrintFilamentList) {
        filamentList.push_back(convertPrintFilamentMmsMappingToJson(filament));
    }
    result.data["mappedFilamentList"] = filamentList;
    result.code = 0;
    result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
    return result;
}
IPCResult PrintSendDialogEx::getPrinterList()
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    IPCResult           result;
    nlohmann::json                  printers    = json::array();
    std::vector<PrinterNetworkInfo> printerList = PrinterManager::getInstance()->getPrinterList();
    for (auto& printer : printerList) {
        nlohmann::json printerJson = json::object();
        printerJson                = convertPrinterNetworkInfoToJson(printer);
        boost::filesystem::path resources_path(Slic3r::resources_dir());
        std::string img_path      = resources_path.string() + "/profiles/" + printer.vendor + "/" + printer.printerModel + "_cover.png";
        printerJson["printerImg"] = PrinterManager::imageFileToBase64DataURI(img_path);
        printers.push_back(printerJson);
    }
    std::string selectedPrinterId = wxGetApp().app_config->get("recent", CONFIG_KEY_SELECTED_PRINTER_ID);
    auto        cfg               = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    std::string printerModel      = "";
    auto        printerModelValue = cfg.option<ConfigOptionString>("printer_model");
    if (printerModelValue) {
        printerModel = printerModelValue->value;
    }
    PrinterNetworkInfo selectedPrinter = PrinterManager::getInstance()->getSelectedPrinter(printerModel, selectedPrinterId);
    for (auto& printer : printers) {
        if (printer["printerId"].get<std::string>() == selectedPrinter.printerId) {
            printer["selected"] = true;
        } else {
            printer["selected"] = false;
        }
    }
    result.data    = printers;
    result.code    = 0;
    result.message = "success";
    return result;
}


IPCResult PrintSendDialogEx::getAdditionalPrinterMmsInfo(const std::string& printerId)
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    IPCResult result;
    result.data                       = json::object();
    result.data["printerId"]          = printerId;
    result.data["mmsInfo"]            = json::object();
    result.data["mappedFilamentList"] = json::array();
    result.data["hasMms"]             = false;

    PrinterNetworkInfo printerNetworkInfo = PrinterManager::getInstance()->getPrinterNetworkInfo(printerId);
    if (printerNetworkInfo.printerId.empty()) {
        result.code    = static_cast<int>(PrinterNetworkErrorCode::PRINTER_NOT_FOUND);
        result.message = getErrorMessage(PrinterNetworkErrorCode::PRINTER_NOT_FOUND);
        return result;
    }

    result.data["printerName"] = printerNetworkInfo.printerName;

    // A printer without an MMS takes the job as sliced; there is nothing to map.
    if (!printerNetworkInfo.systemCapabilities.supportsMultiFilament) {
        result.code    = 0;
        result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
        return result;
    }

    PrinterNetworkResult<PrinterMmsGroup> res = PrinterMmsManager::getInstance()->getPrinterMmsInfo(printerId);
    if (!res.isSuccess()) {
        result.code    = static_cast<int>(res.code);
        result.message = getErrorMessage(res.code);
        return result;
    }

    PrinterMmsGroup mmsGroup = res.data.value();
    bool            hasMms   = !mmsGroup.mmsList.empty() && mmsGroup.connected;
    result.data["mmsInfo"]   = convertPrinterMmsGroupToJson(mmsGroup);
    result.data["hasMms"]    = hasMms;

    // Cleared first: a tray assignment only means something on the machine it came from.
    std::vector<PrintFilamentMmsMapping> filamentList = mPrintFilamentList;
    for (auto& filament : filamentList) {
        filament.mappedMmsFilament = PrinterMmsTray();
        filament.materialOverride  = false;
    }
    if (hasMms) {
        PrinterMmsManager::getInstance()->getFilamentMmsMapping(filamentList, mmsGroup);
    }

    json mapped = json::array();
    for (auto& filament : filamentList) {
        mapped.push_back(convertPrintFilamentMmsMappingToJson(filament));
    }
    result.data["mappedFilamentList"] = mapped;
    result.code                       = 0;
    result.message                    = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
    return result;
}

PrinterNetworkResult<PrintSendDialogEx::ExtendedInfo> PrintSendDialogEx::resolveAdditionalPrinter(const nlohmann::json& printerEntry,
                                                                                                  bool uploadAndPrint)
{
    std::string printerId = printerEntry.value("printerId", std::string());
    if (printerId.empty()) {
        return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_NOT_SELECTED, ExtendedInfo{});
    }

    PrinterNetworkInfo printerNetworkInfo = PrinterManager::getInstance()->getPrinterNetworkInfo(printerId);
    if (printerNetworkInfo.printerId.empty()) {
        return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_NOT_FOUND, ExtendedInfo{});
    }

    // Before the model test and the tray read: a working printer reports a transient tray
    // state, which would surface the refusal as a changed tray. Not conditional on
    // uploadAndPrint - sendPrintFile refuses a busy printer before any bytes move.
    if (printerNetworkInfo.connectStatus != PRINTER_CONNECT_STATUS_CONNECTED) {
        return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_CONNECTION_ERROR, ExtendedInfo{});
    }
    if (printerNetworkInfo.printerStatus != PRINTER_STATUS_IDLE &&
        printerNetworkInfo.printerStatus != PRINTER_STATUS_PRINT_COMPLETED) {
        return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_BUSY, ExtendedInfo{});
    }

    // The dialog blocks these with a confirmation; this is the record of what is printed.
    {
        DynamicPrintConfig cfg        = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        const auto*        modelValue = cfg.option<ConfigOptionString>("printer_model");
        const std::string  projectModel = modelValue ? modelValue->value : std::string();
        if (!projectModel.empty() && !printerNetworkInfo.printerModel.empty() &&
            printerNetworkInfo.printerModel != projectModel) {
            return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_MODEL_NOT_MATCH, ExtendedInfo{});
        }
    }

    // Established here, not taken from the dialog, which reports a failed read as
    // hasMms=false. Read only for a print: an upload does not consume the trays.
    bool            hasMms = false;
    PrinterMmsGroup mmsGroup;
    if (uploadAndPrint && printerNetworkInfo.systemCapabilities.supportsMultiFilament) {
        PrinterNetworkResult<PrinterMmsGroup> res = PrinterMmsManager::getInstance()->getPrinterMmsInfo(printerId);
        if (!res.isSuccess()) {
            // Cannot verify the trays, so cannot vouch for what would be printed.
            return PrinterNetworkResult<ExtendedInfo>(res.code, ExtendedInfo{});
        }
        mmsGroup = res.data.value();
        hasMms   = !mmsGroup.mmsList.empty() && mmsGroup.connected;

        // A multi-filament print needs somewhere to switch.
        if (!hasMms && uploadAndPrint && mPrintFilamentList.size() > 1) {
            return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_MMS_NOT_CONNECTED,
                                                      ExtendedInfo{});
        }
    }

    // This print's filaments, with the tray assignments the dialog chose for this printer.
    std::vector<PrintFilamentMmsMapping> filamentList = mPrintFilamentList;
    for (auto& filament : filamentList) {
        filament.mappedMmsFilament = PrinterMmsTray();
        filament.materialOverride  = false;
    }

    if (hasMms && printerEntry.contains("filamentList") && printerEntry["filamentList"].is_array()) {
        for (auto& printFilament : filamentList) {
            for (const auto& entry : printerEntry["filamentList"]) {
                if (!entry.contains("index") || entry["index"] != printFilament.index) {
                    continue;
                }
                if (!entry.contains("mappedMmsFilament")) {
                    break;
                }
                printFilament.materialOverride = entry.value("materialOverride", false);
                const auto& mapped = entry["mappedMmsFilament"];
                printFilament.mappedMmsFilament.trayName      = mapped.value("trayName", std::string());
                printFilament.mappedMmsFilament.mmsId         = mapped.value("mmsId", std::string());
                printFilament.mappedMmsFilament.trayId        = mapped.value("trayId", std::string());
                printFilament.mappedMmsFilament.filamentColor = mapped.value("filamentColor", std::string());
                printFilament.mappedMmsFilament.filamentName  = mapped.value("filamentName", std::string());
                printFilament.mappedMmsFilament.filamentType  = mapped.value("filamentType", std::string());
                break;
            }
        }
    }

    // An upload does not consume the trays, so an incomplete mapping must not block it.
    if (hasMms && uploadAndPrint) {
        for (const auto& filament : filamentList) {
            if (filament.mappedMmsFilament.trayName.empty() || filament.mappedMmsFilament.mmsId.empty() ||
                filament.mappedMmsFilament.trayId.empty() || filament.mappedMmsFilament.filamentColor.empty() ||
                filament.mappedMmsFilament.filamentName.empty() || filament.mappedMmsFilament.filamentType.empty()) {
                return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_MMS_FILAMENT_NOT_MAPPED, ExtendedInfo{});
            }
            // Indices only reach the printer, so a swapped spool must be caught here.
            const PrinterMmsTray* live = findLiveTray(mmsGroup, filament.mappedMmsFilament);
            if (live == nullptr || !PrinterMmsManager::checkTrayIsReady(*live) ||
                !liveTrayMatchesPick(*live, filament.mappedMmsFilament)) {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__
                                           << boost::format(": tray %s on printer %s no longer holds the selected filament") %
                                                  filament.mappedMmsFilament.trayName % printerId;
                return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_MMS_TRAY_CHANGED,
                                                          ExtendedInfo{});
            }

            // the dialog already checks this, but the send must not trust a stale selection
            if (!filament.materialOverride &&
                !trayMaterialMatches(filament.filamentType, filament.mappedMmsFilament.filamentType)) {
                return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::PRINTER_MMS_FILAMENT_NOT_MAPPED, ExtendedInfo{});
            }
        }
    }

    nlohmann::json mappedFilaments = json::array();
    if (hasMms) {
        for (auto& filament : filamentList) {
            mappedFilaments.push_back(convertPrintFilamentMmsMappingToJson(filament));
        }
    }

    // each additional printer has its own plate; the primary's is used when the entry
    // carries no recognised bedType
    int extraBedType = mBedType;
    {
        const std::string requested = printerEntry.value("bedType", std::string());
        if (requested == "btPC") {
            extraBedType = BedType::btPC;
        } else if (requested == "btPTE") {
            extraBedType = BedType::btPTE;
        }
    }

    // autoRefill is a persistent device setting, not a job parameter; no per-printer control exists yet
    ExtendedInfo extendedInfo = {{"bedType", std::to_string(extraBedType)},
                                 {"timeLapse", mTimeLapse ? "true" : "false"},
                                 {"heatedBedLeveling", mHeatedBedLeveling ? "true" : "false"},
                                 {"hasMms", hasMms ? "true" : "false"},
                                 {"selectedPrinterId", printerId},
                                 // Names this job's machine in the upload queue; without it
                                 // every job shows the same host and filename.
                                 {"printerDisplayName", printerNetworkInfo.printerName},
                                 // Opens this printer's page without pulling focus from the primary.
                                 {"primaryTarget", "false"},
                                 {"filamentAmsMapping", mappedFilaments.dump()}};
    return PrinterNetworkResult<ExtendedInfo>(PrinterNetworkErrorCode::SUCCESS, std::move(extendedInfo));
}

IPCResult PrintSendDialogEx::onPrint(const nlohmann::json& printInfo)
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    IPCResult result;
    result.data                       = nlohmann::json::object();
    PrinterNetworkErrorCode errorCode = PrinterNetworkErrorCode::SUCCESS;
    try {
        mSelectedPrinterId     = "";
        mAdditionalExtendedInfo.clear();
        mDroppedPrinters.clear();
        mTimeLapse             = printInfo["timeLapse"].get<bool>();
        mHeatedBedLeveling     = printInfo["heatedBedLeveling"].get<bool>();
        mAutoRefill            = printInfo["autoRefill"].get<bool>();
        bool uploadAndPrint    = printInfo["uploadAndPrint"].get<bool>();
        mSwitchToDeviceTab = printInfo["switchToDeviceTab"].get<bool>();
        mSelectedPrinterId     = printInfo["selectedPrinterId"].get<std::string>();
        std::string bedType    = printInfo["bedType"].get<std::string>();

        if (bedType == "btPC") {
            mBedType = BedType::btPC;
        } else {
            mBedType = BedType::btPTE;
        }

        if (mSelectedPrinterId.empty()) {
            errorCode      = PrinterNetworkErrorCode::PRINTER_NOT_SELECTED;
            result.message = getErrorMessage(errorCode);
            result.code = static_cast<int>(errorCode);
            return result;
        }

        mPostUploadAction = uploadAndPrint ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None;

        wxString modelName = wxString::FromUTF8(printInfo["modelName"].get<std::string>());
        if (!modelName.EndsWith(".gcode")) {
            modelName += ".gcode";
        }
        mModelName = modelName;
        // txt_filename->SetValue(modelName);

        if (uploadAndPrint && mHasMms) {
            for (auto& printFilament : mPrintFilamentList) {
                // init mappedMmsFilament
                printFilament.mappedMmsFilament = PrinterMmsTray();
                for (int i = 0; i < printInfo["filamentList"].size(); i++) {
                    nlohmann::json mappedFilament = printInfo["filamentList"][i]["mappedMmsFilament"];
                    // update printFilament with mappedFilament
                    if (printInfo["filamentList"][i]["index"] == printFilament.index) {
                        printFilament.materialOverride = printInfo["filamentList"][i].value("materialOverride", false);
                        printFilament.mappedMmsFilament.trayName      = mappedFilament["trayName"];
                        printFilament.mappedMmsFilament.mmsId         = mappedFilament["mmsId"];
                        printFilament.mappedMmsFilament.trayId        = mappedFilament["trayId"];
                        printFilament.mappedMmsFilament.filamentColor = mappedFilament["filamentColor"];
                        printFilament.mappedMmsFilament.filamentName  = mappedFilament["filamentName"];
                        printFilament.mappedMmsFilament.filamentType  = mappedFilament["filamentType"];
                        break;
                    }
                }
            }
            // Re-read: the mapping came from an earlier read. Queried through
            // PrinterManager directly; the wrapper would rebuild the preset map for nothing.
            PrinterMmsGroup primaryMmsGroup;
            {
                PrinterNetworkResult<PrinterMmsGroup> res =
                    PrinterManager::getInstance()->getPrinterMmsInfo(mSelectedPrinterId);
                if (!res.isSuccess()) {
                    errorCode      = res.code;
                    result.message = getErrorMessage(errorCode);
                    result.code    = static_cast<int>(errorCode);
                    return result;
                }
                primaryMmsGroup = res.data.value();
                // mHasMms is left alone: clearing it would skip this block on the retry
                // and send an empty slot map.
                if (primaryMmsGroup.mmsList.empty() || !primaryMmsGroup.connected) {
                    errorCode      = PrinterNetworkErrorCode::PRINTER_MMS_NOT_CONNECTED;
                    result.message = getErrorMessage(errorCode);
                    result.code    = static_cast<int>(errorCode);
                    return result;
                }
            }

            for (auto& printFilament : mPrintFilamentList) {
                if (printFilament.mappedMmsFilament.trayName.empty() || printFilament.mappedMmsFilament.mmsId.empty() ||
                    printFilament.mappedMmsFilament.trayId.empty() || printFilament.mappedMmsFilament.filamentColor.empty() ||
                    printFilament.mappedMmsFilament.filamentName.empty() || printFilament.mappedMmsFilament.filamentType.empty()) {
                    errorCode      = PrinterNetworkErrorCode::PRINTER_MMS_FILAMENT_NOT_MAPPED;
                    result.message = getErrorMessage(errorCode);
                    result.code = static_cast<int>(errorCode);
                    return result;
                }
                // Same live-tray check as resolveAdditionalPrinter.
                const PrinterMmsTray* live = findLiveTray(primaryMmsGroup, printFilament.mappedMmsFilament);
                if (live == nullptr || !PrinterMmsManager::checkTrayIsReady(*live) ||
                    !liveTrayMatchesPick(*live, printFilament.mappedMmsFilament)) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__
                                               << boost::format(": tray %s no longer holds the selected filament") %
                                                      printFilament.mappedMmsFilament.trayName;
                    errorCode      = PrinterNetworkErrorCode::PRINTER_MMS_TRAY_CHANGED;
                    result.message = getErrorMessage(errorCode);
                    result.code    = static_cast<int>(errorCode);
                    return result;
                }

                // Same material check as resolveAdditionalPrinter.
                if (!printFilament.materialOverride &&
                    !trayMaterialMatches(printFilament.filamentType, printFilament.mappedMmsFilament.filamentType)) {
                    errorCode      = PrinterNetworkErrorCode::PRINTER_MMS_FILAMENT_NOT_MAPPED;
                    result.message = getErrorMessage(errorCode);
                    result.code = static_cast<int>(errorCode);
                    return result;
                }
            }
            PrinterMmsManager::getInstance()->saveFilamentMmsMapping(mPrintFilamentList);
        }

        // Backstop for the dialog's per-printer checks; names the printer that failed.
        if (printInfo.contains("additionalPrinters") && printInfo["additionalPrinters"].is_array()) {
            for (const auto& entry : printInfo["additionalPrinters"]) {
                if (!entry.is_object()) {
                    continue;
                }
                std::string extraPrinterId = entry.value("printerId", std::string());
                if (extraPrinterId.empty() || extraPrinterId == mSelectedPrinterId) {
                    continue;
                }

                // dropped, not the whole send; the skipped printers are reported after
                auto extraRes = resolveAdditionalPrinter(entry, uploadAndPrint);
                if (!extraRes.isSuccess()) {
                    PrinterNetworkInfo failedPrinter = PrinterManager::getInstance()->getPrinterNetworkInfo(extraPrinterId);
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__
                                               << boost::format(": skipping printer %s: %s") % extraPrinterId %
                                                      getErrorMessage(extraRes.code);
                    const std::string droppedName = failedPrinter.printerName.empty() ? extraPrinterId
                                                                                     : failedPrinter.printerName;
                    mDroppedPrinters.emplace_back(droppedName, extraRes.message);
                    continue;
                }
                mAdditionalExtendedInfo.push_back(std::move(extraRes.data.value()));
            }
        }
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Print Error: " << e.what();
        errorCode = PrinterNetworkErrorCode::PRINTER_UNKNOWN_ERROR;
    }
    result.code    = errorCode == PrinterNetworkErrorCode::SUCCESS ? 0 : static_cast<int>(errorCode);
    result.message = getErrorMessage(errorCode);
    return result;
}

void PrintSendDialogEx::onCancel() { EndModal(wxID_CANCEL); }

void PrintSendDialogEx::EndModal(int ret)
{
    if (ret == wxID_OK) {
        AppConfig* app_config = wxGetApp().app_config;
        app_config->set("recent", CONFIG_KEY_UPLOADANDPRINT, std::to_string(static_cast<int>(mPostUploadAction)));
        app_config->set("recent", CONFIG_KEY_TIMELAPSE, std::to_string(mTimeLapse));
        app_config->set("recent", CONFIG_KEY_HEATEDBEDLEVELING, std::to_string(mHeatedBedLeveling));
        app_config->set("recent", CONFIG_KEY_BEDTYPE, std::to_string(static_cast<int>(mBedType)));
        app_config->set("recent", CONFIG_KEY_AUTO_REFILL, std::to_string(mAutoRefill));
        app_config->set("recent", CONFIG_KEY_SELECTED_PRINTER_ID, mSelectedPrinterId);
        app_config->set("recent", CONFIG_KEY_SWITCH_TO_DEVICE_TAB, std::to_string(mSwitchToDeviceTab));
    }
    DPIDialog::EndModal(ret);
}

std::vector<PrintSendDialogEx::ExtendedInfo> PrintSendDialogEx::getAdditionalExtendedInfo() const
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    return mAdditionalExtendedInfo;
}

std::vector<std::pair<std::string, std::string>> PrintSendDialogEx::getDroppedPrinters() const
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    return mDroppedPrinters;
}

PrintSendDialogEx::ExtendedInfo PrintSendDialogEx::getExtendedInfo() const
{
    // read on the GUI thread after ShowModal returns, while a late handler may still run
    std::lock_guard<std::mutex> lock(mIpcMutex);
    nlohmann::json filamentList = json::array();
    if (mHasMms) {
        for (auto& filament : mPrintFilamentList) {
            filamentList.push_back(convertPrintFilamentMmsMappingToJson(filament));
        }
    }

    // Names this job's machine in the upload queue; see resolveAdditionalPrinter.
    const PrinterNetworkInfo primaryInfo =
        PrinterManager::getInstance()->getPrinterNetworkInfo(mSelectedPrinterId);

    ExtendedInfo info = {{"bedType", std::to_string(mBedType)},
                         {"timeLapse", mTimeLapse ? "true" : "false"},
                         {"heatedBedLeveling", mHeatedBedLeveling ? "true" : "false"},
                         {"hasMms", mHasMms ? "true" : "false"},
                         {"selectedPrinterId", mSelectedPrinterId},
                         {"printerDisplayName", primaryInfo.printerName},
                         {"filamentAmsMapping", filamentList.dump()}};

    // autoRefill persists on the printer; only send it when the toggle was shown (supportsAutoRefill)
    if (primaryInfo.printCapabilities.supportsAutoRefill) {
        info["autoRefill"] = mAutoRefill ? "true" : "false";
    }
    return info;
}

PrintHostPostUploadAction PrintSendDialogEx::getPostAction() const
{
    return mPostUploadAction;
}

bool PrintSendDialogEx::getSwitchToDeviceTab() const
{
    return mSwitchToDeviceTab;
}

void PrintSendDialogEx::OnCloseWindow(wxCloseEvent& event)
{
    // // If async operation is in progress, prevent closing
    // if (mAsyncOperationInProgress && !mIsDestroying) {
    //     // Show a message to user that operation is in progress
    //     // wxMessageBox(_L("Print task preparation is in progress. Please wait..."),
    //     //              _L("Operation in Progress"), wxOK | wxICON_INFORMATION);
    //     event.Veto(); // Prevent the window from closing
    //     return;
    // }

    // Allow normal close behavior
    event.Skip();
}

BedType PrintSendDialogEx::getCurrentBedType() const
{
    std::lock_guard<std::mutex> lock(mIpcMutex);
    std::string str_bed_type = wxGetApp().app_config->get("curr_bed_type");
    int         bedType      = atoi(str_bed_type.c_str());
    return static_cast<BedType>(bedType);
}
}} // namespace Slic3r::GUI
