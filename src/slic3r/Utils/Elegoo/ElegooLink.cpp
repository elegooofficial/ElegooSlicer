#include "ElegooLink.hpp"
#include "PrinterNetworkEvent.hpp"
#include "libslic3r/PrinterNetworkResult.hpp"
#include "libslic3r/PrinterNetworkInfo.hpp"
#include "elegoolink/elegoo_link.h"
#include "libslic3r/Utils.hpp"
#include <thread>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include "libslic3r_version.h"
#define ELEGOO_NETWORK_LIBRARY "ElegooNetwork"

// Do not lock mMutex: uninit() may block in SDK cleanup while other threads still hit this check.
#define CHECK_INITIALIZED(returnValue) \
    do { \
        if (!mIsInitialized.load(std::memory_order_acquire)) { \
            return PrinterNetworkResult<std::decay_t<decltype(returnValue)>>(PrinterNetworkErrorCode::PRINTER_NETWORK_NOT_INITIALIZED, \
                                                                             returnValue); \
        } \
    } while (0)

namespace Slic3r {

namespace {

std::string get_cloud_os_name()
{
#if defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
    return "mac";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string get_cloud_arch_name()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string build_cloud_user_agent()
{
    return std::string("ElegooSlicer/") + ELEGOOSLICER_VERSION + " (" + get_cloud_os_name() + "; " + get_cloud_arch_name() + ")";
}

// Map log level string to spdlog log level: trace=0, debug=1, info=2, warn=3, error=4, critical=5, off=6
unsigned int level_string_to_spdlog(const std::string& level)
{
    static const std::map<std::string, int> levelMap = {
        {"trace", 0},
        {"debug", 1},
        {"info", 2},
        {"warn", 3},
        {"warning", 3},  // alias for warn
        {"error", 4},
        {"critical", 5},
        {"fatal", 5},    // alias for critical
        {"off", 6}
    };
    
    auto it = levelMap.find(level);
    if (it != levelMap.end()) {
        return it->second;
    }
    // Default to info level if not found
    return 2;
}

PrinterNetworkErrorCode parseElegooResult(elink::ELINK_ERROR_CODE code)
{
    switch (code) {
    case elink::ELINK_ERROR_CODE::SUCCESS: return PrinterNetworkErrorCode::SUCCESS;
    case elink::ELINK_ERROR_CODE::UNKNOWN_ERROR: return PrinterNetworkErrorCode::UNKNOWN_ERROR;
    case elink::ELINK_ERROR_CODE::NOT_INITIALIZED: return PrinterNetworkErrorCode::NOT_INITIALIZED;
    case elink::ELINK_ERROR_CODE::INVALID_PARAMETER: return PrinterNetworkErrorCode::INVALID_PARAMETER;
    case elink::ELINK_ERROR_CODE::OPERATION_TIMEOUT: return PrinterNetworkErrorCode::OPERATION_TIMEOUT;
    case elink::ELINK_ERROR_CODE::OPERATION_CANCELLED: return PrinterNetworkErrorCode::OPERATION_CANCELLED;
    case elink::ELINK_ERROR_CODE::OPERATION_IN_PROGRESS: return PrinterNetworkErrorCode::OPERATION_IN_PROGRESS;
    case elink::ELINK_ERROR_CODE::OPERATION_NOT_IMPLEMENTED: return PrinterNetworkErrorCode::OPERATION_NOT_IMPLEMENTED;
    case elink::ELINK_ERROR_CODE::NETWORK_ERROR: return PrinterNetworkErrorCode::NETWORK_ERROR;
    case elink::ELINK_ERROR_CODE::INSUFFICIENT_MEMORY: return PrinterNetworkErrorCode::INSUFFICIENT_MEMORY;
    case elink::ELINK_ERROR_CODE::INVALID_USERNAME_OR_PASSWORD: return PrinterNetworkErrorCode::INVALID_USERNAME_OR_PASSWORD;
    case elink::ELINK_ERROR_CODE::INVALID_TOKEN: return PrinterNetworkErrorCode::INVALID_TOKEN;
    case elink::ELINK_ERROR_CODE::INVALID_ACCESS_CODE: return PrinterNetworkErrorCode::INVALID_ACCESS_CODE;
    case elink::ELINK_ERROR_CODE::INVALID_PIN_CODE: return PrinterNetworkErrorCode::INVALID_PIN_CODE;
    case elink::ELINK_ERROR_CODE::FILE_TRANSFER_FAILED: return PrinterNetworkErrorCode::FILE_TRANSFER_FAILED;
    case elink::ELINK_ERROR_CODE::FILE_NOT_FOUND: return PrinterNetworkErrorCode::FILE_NOT_FOUND;
    case elink::ELINK_ERROR_CODE::FILE_ALREADY_EXISTS: return PrinterNetworkErrorCode::FILE_ALREADY_EXISTS;
    case elink::ELINK_ERROR_CODE::FILE_ACCESS_DENIED: return PrinterNetworkErrorCode::FILE_ACCESS_DENIED;
    case elink::ELINK_ERROR_CODE::PRINTER_NOT_FOUND: return PrinterNetworkErrorCode::PRINTER_NOT_FOUND;
    case elink::ELINK_ERROR_CODE::PRINTER_CONNECTION_ERROR: return PrinterNetworkErrorCode::PRINTER_CONNECTION_ERROR;
    case elink::ELINK_ERROR_CODE::PRINTER_CONNECTION_LIMIT_EXCEEDED: return PrinterNetworkErrorCode::PRINTER_CONNECTION_LIMIT_EXCEEDED;
    case elink::ELINK_ERROR_CODE::PRINTER_ALREADY_CONNECTED: return PrinterNetworkErrorCode::PRINTER_ALREADY_CONNECTED;
    case elink::ELINK_ERROR_CODE::PRINTER_BUSY: return PrinterNetworkErrorCode::PRINTER_BUSY;
    case elink::ELINK_ERROR_CODE::PRINTER_COMMAND_FAILED: return PrinterNetworkErrorCode::PRINTER_COMMAND_FAILED;
    case elink::ELINK_ERROR_CODE::PRINTER_UNKNOWN_ERROR: return PrinterNetworkErrorCode::PRINTER_UNKNOWN_ERROR;
    case elink::ELINK_ERROR_CODE::PRINTER_INVALID_PARAMETER: return PrinterNetworkErrorCode::PRINTER_INVALID_PARAMETER;
    case elink::ELINK_ERROR_CODE::PRINTER_INVALID_RESPONSE: return PrinterNetworkErrorCode::PRINTER_INVALID_RESPONSE;
    case elink::ELINK_ERROR_CODE::PRINTER_ACCESS_DENIED: return PrinterNetworkErrorCode::PRINTER_ACCESS_DENIED;
    case elink::ELINK_ERROR_CODE::PRINTER_PRINT_FILE_NOT_FOUND: return PrinterNetworkErrorCode::PRINTER_PRINT_FILE_NOT_FOUND;
    case elink::ELINK_ERROR_CODE::PRINTER_MISSING_BED_LEVELING_DATA: return PrinterNetworkErrorCode::PRINTER_MISSING_BED_LEVELING_DATA;
    case elink::ELINK_ERROR_CODE::PRINTER_OFFLINE: return PrinterNetworkErrorCode::PRINTER_OFFLINE;
    case elink::ELINK_ERROR_CODE::PRINTER_FILAMENT_RUNOUT: return PrinterNetworkErrorCode::PRINTER_FILAMENT_RUNOUT;
    case elink::ELINK_ERROR_CODE::SERVER_UNKNOWN_ERROR: return PrinterNetworkErrorCode::SERVER_UNKNOWN_ERROR;
    case elink::ELINK_ERROR_CODE::SERVER_INVALID_RESPONSE: return PrinterNetworkErrorCode::SERVER_INVALID_RESPONSE;
    case elink::ELINK_ERROR_CODE::SERVER_TOO_MANY_REQUESTS: return PrinterNetworkErrorCode::SERVER_TOO_MANY_REQUESTS;
    case elink::ELINK_ERROR_CODE::SERVER_RTM_NOT_CONNECTED: return PrinterNetworkErrorCode::SERVER_RTM_NOT_CONNECTED;
    case elink::ELINK_ERROR_CODE::SERVER_UNAUTHORIZED: return PrinterNetworkErrorCode::SERVER_UNAUTHORIZED;
    case elink::ELINK_ERROR_CODE::SERVER_FORBIDDEN: return PrinterNetworkErrorCode::SERVER_FORBIDDEN;
    case elink::ELINK_ERROR_CODE::SERVER_PIN_CODE_MISMATCH: return PrinterNetworkErrorCode::SERVER_PIN_CODE_MISMATCH;
    default: return PrinterNetworkErrorCode::UNKNOWN_ERROR;
    }
}

PrinterStatus parseElegooStatus(elink::PrinterState mainStatus, elink::PrinterSubState subStatus)
{
    PrinterStatus printerStatus = PRINTER_STATUS_UNKNOWN;
    switch (mainStatus) {
    case elink::PrinterState::OFFLINE: printerStatus = PRINTER_STATUS_OFFLINE; break;
    case elink::PrinterState::IDLE: printerStatus = PRINTER_STATUS_IDLE; break;
    case elink::PrinterState::PRINTING: printerStatus = PRINTER_STATUS_PRINTING; break;
    case elink::PrinterState::SELF_CHECKING: printerStatus = PRINTER_STATUS_SELF_CHECKING; break;
    case elink::PrinterState::AUTO_LEVELING: printerStatus = PRINTER_STATUS_AUTO_LEVELING; break;
    case elink::PrinterState::PID_CALIBRATING: printerStatus = PRINTER_STATUS_PID_CALIBRATING; break;
    case elink::PrinterState::RESONANCE_TESTING: printerStatus = PRINTER_STATUS_RESONANCE_TESTING; break;
    case elink::PrinterState::UPDATING: printerStatus = PRINTER_STATUS_UPDATING; break;
    case elink::PrinterState::FILE_COPYING: printerStatus = PRINTER_STATUS_FILE_COPYING; break;
    case elink::PrinterState::FILE_TRANSFERRING: printerStatus = PRINTER_STATUS_FILE_TRANSFERRING; break;
    case elink::PrinterState::HOMING: printerStatus = PRINTER_STATUS_HOMING; break;
    case elink::PrinterState::PREHEATING: printerStatus = PRINTER_STATUS_PREHEATING; break;
    case elink::PrinterState::FILAMENT_OPERATING: printerStatus = PRINTER_STATUS_FILAMENT_OPERATING; break;
    case elink::PrinterState::EXTRUDER_OPERATING: printerStatus = PRINTER_STATUS_EXTRUDER_OPERATING; break;
    case elink::PrinterState::UNKNOWN: printerStatus = PRINTER_STATUS_UNKNOWN; break;
    case elink::PrinterState::BUSY: printerStatus = PRINTER_STATUS_BUSY; break;
    case elink::PrinterState::EXCEPTION: printerStatus = PRINTER_STATUS_ERROR; break;
    case elink::PrinterState::VIDEO_COMPOSING: printerStatus = PRINTER_STATUS_VIDEO_COMPOSING; break;
    case elink::PrinterState::EMERGENCY_STOP: printerStatus = PRINTER_STATUS_EMERGENCY_STOP; break;
    case elink::PrinterState::POWER_LOSS_RECOVERY: printerStatus = PRINTER_STATUS_POWER_LOSS_RECOVERY; break;
    case elink::PrinterState::INITIALIZING: printerStatus = PRINTER_STATUS_INITIALIZING; break;
    default: printerStatus = PRINTER_STATUS_UNKNOWN; break;
    }

    switch (subStatus) {
    case elink::PrinterSubState::P_PAUSING: printerStatus = PRINTER_STATUS_PAUSING; break;
    case elink::PrinterSubState::P_PAUSED: printerStatus = PRINTER_STATUS_PAUSED; break;
    case elink::PrinterSubState::P_PRINTING_COMPLETED: printerStatus = PRINTER_STATUS_PRINT_COMPLETED; break;
    default: break;
    }
    return printerStatus;
}

PrinterNetworkInfo convertFromElegooPrinterInfo(const elink::PrinterInfo& printerInfo)
{
    PrinterNetworkInfo info;
    info.printerId       = printerInfo.printerId;
    info.firmwareVersion = printerInfo.firmwareVersion;
    info.mainboardId     = printerInfo.mainboardId;
    info.serialNumber    = printerInfo.serialNumber;
    info.webUrl          = printerInfo.webUrl;
    info.printerName     = printerInfo.name;
    info.host            = printerInfo.host;
    info.printerModel    = printerInfo.model;
    info.vendor          = printerInfo.brand;
    info.networkType     = printerInfo.networkMode == elink::NetworkMode::LAN ? NETWORK_TYPE_LAN : NETWORK_TYPE_WAN;
    // std map 转json
    nlohmann::json json;
    for (const auto& [key, value] : printerInfo.extraInfo) {
        json[key] = value;
    }
    info.extraInfo = json.dump();

    info.authMode = PRINTER_AUTH_MODE_NONE;
    if (printerInfo.authMode == "basic") {
        info.authMode = PRINTER_AUTH_MODE_PASSWORD;
    } else if (printerInfo.authMode == "accessCode") {
        info.authMode = PRINTER_AUTH_MODE_ACCESS_CODE;
    } else if (printerInfo.authMode == "pinCode") {
        info.authMode = PRINTER_AUTH_MODE_PIN_CODE;
    }

    if (info.networkType == NETWORK_TYPE_WAN) {
        info.authMode = PRINTER_AUTH_MODE_PIN_CODE;
    }

    return info;
}
PrinterNetworkInfo convertFromElegooPrinterAttributes(const elink::PrinterAttributes& attributes)
{
    PrinterNetworkInfo info = convertFromElegooPrinterInfo(attributes);

    info.printCapabilities.supportsAutoBedLeveling    = attributes.capabilities.printCapabilities.supportsAutoBedLeveling;
    info.printCapabilities.supportsTimeLapse          = attributes.capabilities.printCapabilities.supportsTimeLapse;
    info.printCapabilities.supportsHeatedBedSwitching = attributes.capabilities.printCapabilities.supportsHeatedBedSwitching;
    info.printCapabilities.supportsFilamentMapping    = attributes.capabilities.printCapabilities.supportsFilamentMapping;
    info.printCapabilities.supportsAutoRefill         = attributes.capabilities.printCapabilities.supportsAutoRefill;
    info.systemCapabilities.supportsMultiFilament     = attributes.capabilities.systemCapabilities.supportsMultiFilament;
    info.systemCapabilities.canGetDiskInfo            = attributes.capabilities.systemCapabilities.canGetDiskInfo;
    info.systemCapabilities.canSetPrinterName         = attributes.capabilities.systemCapabilities.canSetPrinterName;

    return info;
}

elink::PrinterType getPrinterType(const PrinterNetworkInfo& printerNetworkInfo)
{
    std::string model  = printerNetworkInfo.printerModel;
    std::string vendor = printerNetworkInfo.vendor;
    std::transform(model.begin(), model.end(), model.begin(), ::tolower);
    std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
    if (model.find(vendor) == std::string::npos) {
        model = vendor + " " + model;
    }
    if (model == "elegoo centauri carbon 2" || model == "elegoo centauri 2") {
        return elink::PrinterType::ELEGOO_FDM_CC2;
    } else if (model == "elegoo centauri carbon" || model == "elegoo centauri") {
        return elink::PrinterType::ELEGOO_FDM_CC;
    } else if (vendor == "elegoo") {
        return elink::PrinterType::ELEGOO_FDM_KLIPPER;
    }

    return elink::PrinterType::GENERIC_FDM_KLIPPER;
}

} // namespace

ElegooLink::ElegooLink() {}

ElegooLink::~ElegooLink() {}

void ElegooLink::init(const std::string& region, std::string& iotUrl, const std::string& logLevel)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mIsInitialized) {
        BOOST_LOG_TRIVIAL(info) << "ElegooLink::init: already initialized";
        return;
    }

    elink::ElegooLink::Config cfg;

    // Map log level string to spdlog log level: trace=0, debug=1, info=2, warn=3, error=4, critical=5, off=6
    cfg.log.logLevel = level_string_to_spdlog(logLevel);
    cfg.log.logEnableConsole = true;
    cfg.log.logEnableFile = true;
    cfg.log.logFileName = data_dir() + "/log/elegoolink.log";
    cfg.log.logMaxFileSize = 10 * 1024 * 1024;
    cfg.log.logMaxFiles = 5;
    auto webDir = resources_dir();
    std::replace(webDir.begin(), webDir.end(), '\\', '/');
    cfg.local.staticWebPath = webDir + "/plugins/elegoolink/web";
    cfg.cloud.staticWebPath = cfg.local.staticWebPath;
    std::string caCertDir = resources_dir();
    std::replace(caCertDir.begin(), caCertDir.end(), '\\', '/');
    cfg.cloud.caCertPath = caCertDir + "/cert/cacert.pem"; // Use system default CA certs
    cfg.cloud.rtmLogPath = data_dir() + "/log/rtm.log";

    cfg.cloud.userAgent = build_cloud_user_agent();
    cfg.cloud.region = region;
    cfg.cloud.baseApiUrl = iotUrl;
    if (!elink::ElegooLink::getInstance().initialize(cfg)) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": error initializing ElegooLink";
    }
    // elink::SetRegionParams setRegionParams;
    // setRegionParams.region = region;
    // setRegionParams.baseUrl = iotUrl;
    // setRegionParams.caCertPath = cfg.cloud.caCertPath;
    // elink::ElegooLink::getInstance().setRegion(setRegionParams);

    std::string version = elink::ElegooLink::getInstance().getVersion();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": version: %s") % version;
    // Subscribe to ElegooLink events
    elink::ElegooLink::getInstance().subscribeEvent<elink::PrinterConnectionEvent>(
        [&](const std::shared_ptr<elink::PrinterConnectionEvent>& event) {
            PrinterConnectStatus status = (event->connectionStatus.status == elink::ConnectionStatus::CONNECTED) ?
                                              PRINTER_CONNECT_STATUS_CONNECTED :
                                              PRINTER_CONNECT_STATUS_DISCONNECTED;

            PrinterNetworkEvent::getInstance()->connectStatusChanged.emit(
                PrinterConnectStatusEvent(event->connectionStatus.printerId, status));
        });

    // print status and print task changed event
    elink::ElegooLink::getInstance().subscribeEvent<elink::PrinterStatusEvent>([&](const std::shared_ptr<elink::PrinterStatusEvent>& event) {
        PrinterStatus status = parseElegooStatus(event->status.printerStatus.state, event->status.printerStatus.subState);
        int deviceAssistantStatus = event->status.deviceAssistantStatus;
        std::vector<PrinterExceptionDetail> exceptions;
        exceptions.reserve(event->status.exceptions.size());
        for (const auto& exceptionDetail : event->status.exceptions) {
            PrinterExceptionDetail detail;
            detail.code  = exceptionDetail.code;
            exceptions.push_back(detail);
        }
        PrinterNetworkEvent::getInstance()->statusChanged.emit(
            Slic3r::PrinterStatusEvent(event->status.printerId, status, exceptions, deviceAssistantStatus));

        PrinterPrintTask task;
        task.taskId        = event->status.printStatus.taskId;
        task.fileName      = event->status.printStatus.fileName;
        task.totalTime     = event->status.printStatus.totalTime;
        task.currentTime   = event->status.printStatus.currentTime;
        task.estimatedTime = event->status.printStatus.estimatedTime;
        task.progress      = event->status.printStatus.progress;

        PrinterNetworkEvent::getInstance()->printTaskChanged.emit(PrinterPrintTaskEvent(event->status.printerId, task));
    });

    elink::ElegooLink::getInstance().subscribeEvent<elink::PrinterAttributesEvent>(
        [&](const std::shared_ptr<elink::PrinterAttributesEvent>& event) {
            PrinterNetworkInfo info = convertFromElegooPrinterAttributes(event->attributes);
            PrinterNetworkEvent::getInstance()->attributesChanged.emit(
                PrinterAttributesEvent(event->attributes.printerId, info));
        });

    // web pass-through data
    elink::ElegooLink::getInstance().subscribeEvent<elink::PrinterEventRawEvent>(
        [&](const std::shared_ptr<elink::PrinterEventRawEvent>& event) {
            PrinterNetworkEvent::getInstance()->eventRawChanged.emit(
                PrinterEventRawEvent(event->rawData.printerId, event->rawData.rawData));
        });
    // user network event(iot)
    elink::ElegooLink::getInstance().subscribeEvent<elink::RtmMessageEvent>([&](const std::shared_ptr<elink::RtmMessageEvent>& event) {
        UserNetworkEvent::getInstance()->rtmMessageChanged.emit(
            UserRtmMessageEvent(event->message.printerId, event->message.message));
    });
    elink::ElegooLink::getInstance().subscribeEvent<elink::RtcTokenEvent>([&](const std::shared_ptr<elink::RtcTokenEvent>& event) {
        UserNetworkInfo userInfo;
        userInfo.userId             = event->token.userId;
        userInfo.rtcToken           = event->token.rtcToken;
        userInfo.rtcTokenExpireTime = event->token.rtcTokenExpireTime;
        UserNetworkEvent::getInstance()->rtcTokenChanged.emit(UserRtcTokenEvent(userInfo));
    });
    elink::ElegooLink::getInstance().subscribeEvent<elink::LoggedInElsewhereEvent>(
        [&](const std::shared_ptr<elink::LoggedInElsewhereEvent>& event) {
            UserNetworkEvent::getInstance()->loggedInElsewhereChanged.emit(UserLoggedInElsewhereEvent());
        });

    elink::ElegooLink::getInstance().subscribeEvent<elink::OnlineStatusChangedEvent>(
        [&](const std::shared_ptr<elink::OnlineStatusChangedEvent>& event) {
            UserNetworkEvent::getInstance()->onlineStatusChanged.emit(UserOnlineStatusChangedEvent(event->isOnline));
        });


    elink::ElegooLink::getInstance().subscribeEvent<elink::PrinterListChangedEvent>(
        [&](const std::shared_ptr<elink::PrinterListChangedEvent>& event) {
            PrinterNetworkEvent::getInstance()->printerOnlineListChanged.emit(PrinterOnlineListChangedEvent());
        });
    mIsInitialized.store(true, std::memory_order_release);
    BOOST_LOG_TRIVIAL(info) << "ElegooLink::init: complete (region=" << region << ")";
}

void ElegooLink::uninit()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mIsInitialized.load(std::memory_order_acquire)) {
        BOOST_LOG_TRIVIAL(info) << "ElegooLink::uninit: not initialized, skip";
        return;
    }
    // Unsubscribe LAN events

    elink::ElegooLink::getInstance().clearAllEventSubscriptions();
    // Cleanup LAN link
    elink::ElegooLink::getInstance().cleanup();

    mIsInitialized.store(false, std::memory_order_release);
    BOOST_LOG_TRIVIAL(info) << "ElegooLink::uninit: complete";
}

std::string parseUnknownErrorMsg(PrinterNetworkErrorCode resultCode, const std::string& msg)
{
    if (resultCode == PrinterNetworkErrorCode::PRINTER_UNKNOWN_ERROR || resultCode == PrinterNetworkErrorCode::SERVER_UNKNOWN_ERROR ||
        resultCode == PrinterNetworkErrorCode::UNKNOWN_ERROR) {
        if (msg.find("[ErrorCode:") != std::string::npos) {
            return getErrorMessage(resultCode) + "(" +
                   msg.substr(msg.find("[ErrorCode:") + 11, msg.find("]") - msg.find("[ErrorCode:") - 11) + ")";
        }
    }
    return "";
}

PrinterNetworkResult<PrinterNetworkInfo> ElegooLink::connectToPrinter(const PrinterNetworkInfo& printerNetworkInfo)
{
    CHECK_INITIALIZED(printerNetworkInfo);
    PrinterNetworkErrorCode     resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::ConnectPrinterResult elinkResult;
    try {
        elink::ConnectPrinterParams connectionParams;
        connectionParams.printerId   = printerNetworkInfo.printerId;
        connectionParams.printerType = getPrinterType(printerNetworkInfo);
        connectionParams.name        = printerNetworkInfo.printerName;
        if (printerNetworkInfo.authMode == PRINTER_AUTH_MODE_PASSWORD) {
            connectionParams.authMode = "basic";
        } else if (printerNetworkInfo.authMode == PRINTER_AUTH_MODE_ACCESS_CODE) {
            connectionParams.authMode = "accessCode";
        } else if (printerNetworkInfo.authMode == PRINTER_AUTH_MODE_PIN_CODE) {
            connectionParams.authMode = "pinCode";
        } else {
            connectionParams.authMode = "";
        }
        connectionParams.username          = printerNetworkInfo.username;
        connectionParams.password          = printerNetworkInfo.password;
        connectionParams.host              = printerNetworkInfo.host;
        connectionParams.model             = printerNetworkInfo.printerModel;
        connectionParams.serialNumber      = printerNetworkInfo.serialNumber;
        connectionParams.brand             = printerNetworkInfo.vendor;
        connectionParams.autoReconnect     = true;
        connectionParams.checkConnection   = true;
        connectionParams.connectionTimeout = 5 * 1000;
        connectionParams.token             = printerNetworkInfo.token;
        connectionParams.accessCode        = printerNetworkInfo.accessCode;
        connectionParams.pinCode           = printerNetworkInfo.pinCode;
        connectionParams.webUrl            = printerNetworkInfo.webUrl;
        connectionParams.networkMode        = (printerNetworkInfo.networkType == NETWORK_TYPE_LAN) ? elink::NetworkMode::LAN : elink::NetworkMode::CLOUD;
        elinkResult = elink::ElegooLink::getInstance().connectPrinter(connectionParams);

        resultCode = parseElegooResult(elinkResult.code);
        if (elinkResult.code == elink::ELINK_ERROR_CODE::SUCCESS) {
            if (elinkResult.data.has_value() && elinkResult.data.value().isConnected &&
                elinkResult.data.value().printerInfo.printerId == printerNetworkInfo.printerId) {
                auto               addPrinter = elinkResult.data.value().printerInfo;
                PrinterNetworkInfo info       = convertFromElegooPrinterInfo(addPrinter);
                return PrinterNetworkResult<PrinterNetworkInfo>(resultCode, info, parseUnknownErrorMsg(resultCode, elinkResult.message));
            } else {
                resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_INVALID_DATA;
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }

    return PrinterNetworkResult<PrinterNetworkInfo>(resultCode, printerNetworkInfo, parseUnknownErrorMsg(resultCode, elinkResult.message));
}
PrinterNetworkResult<PrinterNetworkInfo> ElegooLink::bindWANPrinter(const PrinterNetworkInfo& printerNetworkInfo)
{
    CHECK_INITIALIZED(printerNetworkInfo);
    PrinterNetworkErrorCode  resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::BindPrinterResult elinkResult;
    try {
        elink::BindPrinterParams elinkParams;
        elinkParams.name         = printerNetworkInfo.printerName;
        std::string printerModel = printerNetworkInfo.printerModel;
        if (boost::to_upper_copy(printerNetworkInfo.printerModel).find(boost::to_upper_copy(printerNetworkInfo.vendor)) !=
            std::string::npos) {
            printerModel = boost::algorithm::ireplace_first_copy(printerNetworkInfo.printerModel, printerNetworkInfo.vendor, "");
        }
        elinkParams.model        = boost::trim_copy(printerModel);
        elinkParams.serialNumber = printerNetworkInfo.serialNumber;
        elinkParams.pinCode      = printerNetworkInfo.pinCode;

        if (printerNetworkInfo.authMode == PRINTER_AUTH_MODE_PASSWORD) {
            elinkParams.authMode = "basic";
        } else if (printerNetworkInfo.authMode == PRINTER_AUTH_MODE_ACCESS_CODE) {
            elinkParams.authMode = "accessCode";
        } else if (printerNetworkInfo.authMode == PRINTER_AUTH_MODE_PIN_CODE) {
            elinkParams.authMode = "pinCode";
        } else {
            elinkParams.authMode = "";
        }

        elinkResult = elink::ElegooLink::getInstance().bindPrinter(elinkParams);
        resultCode  = parseElegooResult(elinkResult.code);
        if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
            if (elinkResult.data.has_value() && elinkResult.data.value().bindResult) {
                PrinterNetworkInfo boundInfo = printerNetworkInfo;
                PrinterNetworkInfo resBoundInfo = convertFromElegooPrinterInfo(elinkResult.data.value().printerInfo);
                boundInfo.serialNumber = resBoundInfo.serialNumber;
                boundInfo.printerId = resBoundInfo.printerId;
                boundInfo.printerName = resBoundInfo.printerName;
                boundInfo.protocolVersion = resBoundInfo.protocolVersion;
                boundInfo.firmwareVersion = resBoundInfo.firmwareVersion;
                boundInfo.mainboardId = resBoundInfo.mainboardId;
                boundInfo.webUrl = resBoundInfo.webUrl;
                return PrinterNetworkResult<PrinterNetworkInfo>(resultCode, boundInfo,
                                                                parseUnknownErrorMsg(resultCode, elinkResult.message));
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }
    return PrinterNetworkResult<PrinterNetworkInfo>(resultCode, printerNetworkInfo, parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<bool> ElegooLink::disconnectFromPrinter(const std::string& printerId)
{
    CHECK_INITIALIZED(false);
    PrinterNetworkErrorCode resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::VoidResult       elinkResult;
    try {
        elinkResult = elink::ElegooLink::getInstance().disconnectPrinter(printerId);
        resultCode  = parseElegooResult(elinkResult.code);

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }

    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<bool> ElegooLink::unbindWANPrinter(const std::string& serialNumber)
{
    CHECK_INITIALIZED(false);
    PrinterNetworkErrorCode    resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::VoidResult          elinkResult;
    elink::UnbindPrinterParams params;
    params.serialNumber = serialNumber;
    elinkResult         = elink::ElegooLink::getInstance().unbindPrinter(params);
    resultCode          = parseElegooResult(elinkResult.code);
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<bool> ElegooLink::setRegion(const std::string& region, const std::string& iotUrl)
{
    CHECK_INITIALIZED(false);
    elink::SetRegionParams params;
    params.region  = region;
    params.baseUrl = iotUrl;
    std::string caCertDir = resources_dir();
    std::replace(caCertDir.begin(), caCertDir.end(), '\\', '/');
    params.caCertPath      =  caCertDir + "/cert/cacert.pem"; // Use system default CA certs
    elink::VoidResult       elinkResult = elink::ElegooLink::getInstance().setRegion(params);
    PrinterNetworkErrorCode resultCode  = parseElegooResult(elinkResult.code);
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<std::vector<PrinterNetworkInfo>> ElegooLink::discoverPrinters()
{
    CHECK_INITIALIZED(std::vector<PrinterNetworkInfo>());
    std::vector<PrinterNetworkInfo> discoverPrinters;
    PrinterNetworkErrorCode         resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::PrinterDiscoveryResult   elinkResult;
    try {
        elink::PrinterDiscoveryParams discoveryParams;
        discoveryParams.timeoutMs         = 8000;
        discoveryParams.broadcastInterval = 2000;
        discoveryParams.enableAutoRetry   = true;
        elinkResult                       = elink::ElegooLink::getInstance().startPrinterDiscovery(discoveryParams);
        resultCode                        = parseElegooResult(elinkResult.code);
        if (elinkResult.code == elink::ELINK_ERROR_CODE::SUCCESS && elinkResult.data.has_value()) {
            for (const auto& printer : elinkResult.value().printers) {
                PrinterNetworkInfo info = convertFromElegooPrinterInfo(printer);
                discoverPrinters.push_back(info);
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }

    return PrinterNetworkResult<std::vector<PrinterNetworkInfo>>(resultCode, discoverPrinters,
                                                                 parseUnknownErrorMsg(resultCode, elinkResult.message));
}

bool ElegooLink::isBusy(const std::string& printerId, PrinterStatus& status, int tryCount)
{
    if (tryCount < 1) {
        tryCount = 1;
    }
    bool isBusy = true;
    status      = PRINTER_STATUS_UNKNOWN;
    for (int i = 0; i < tryCount; i++) {
        // wait between polls, not before the first one: an idle printer answers immediately
        if (i > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        auto elinkResult = elink::ElegooLink::getInstance().getPrinterStatus({printerId});
        if (elinkResult.code == elink::ELINK_ERROR_CODE::SUCCESS) {
            const auto& statusData = elinkResult.value();
            if (statusData.printerStatus.state == elink::PrinterState::IDLE ||
                statusData.printerStatus.subState == elink::PrinterSubState::P_PRINTING_COMPLETED) {
                isBusy = false;
                status = PRINTER_STATUS_IDLE;
                if(statusData.printerStatus.subState == elink::PrinterSubState::P_PRINTING_COMPLETED){
                    status = PRINTER_STATUS_PRINT_COMPLETED;
                }
                break;
            } else {
                status = parseElegooStatus(statusData.printerStatus.state, statusData.printerStatus.subState);
            }
        } else {
            return false;
        }
    }
    return isBusy;
}

PrinterNetworkResult<bool> ElegooLink::sendPrintTask(const PrinterNetworkParams& params)
{
    CHECK_INITIALIZED(false);
    PrinterNetworkErrorCode resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::StartPrintResult elinkResult;
    try {
        PrinterStatus status;
        if (isBusy(params.printerId, status, 10)) {
            return PrinterNetworkResult<bool>(PrinterNetworkErrorCode::PRINTER_BUSY, false,
                                              "Printer is busy, status: " + std::to_string(status));
        }
        elink::StartPrintParams startPrintParams;
        startPrintParams.printerId       = params.printerId;
        startPrintParams.storageLocation = "local";
        startPrintParams.fileName        = params.fileName;
        startPrintParams.autoBedLeveling = params.heatedBedLeveling;
        startPrintParams.heatedBedType   = params.bedType;
        startPrintParams.enableTimeLapse = params.timeLapse;
        startPrintParams.slotMap.clear();

        for (const auto& filamentMmsMapping : params.filamentMmsMappingList) {
            if (filamentMmsMapping.mappedMmsFilament.mmsId.empty() || filamentMmsMapping.mappedMmsFilament.trayId.empty()) {
                continue;
            }
            elink::SlotMapItem slotMapItem;
            slotMapItem.t        = filamentMmsMapping.index;
            slotMapItem.canvasId = std::stoi(filamentMmsMapping.mappedMmsFilament.mmsId);
            slotMapItem.trayId   = std::stoi(filamentMmsMapping.mappedMmsFilament.trayId);
            startPrintParams.slotMap.push_back(slotMapItem);
        }

        elink::VoidResult autoRefillResult;

        if (params.hasMms) {
            if (params.autoRefill) {
                autoRefillResult = elink::ElegooLink::getInstance().setAutoRefill({params.printerId, true});
            } else {
                autoRefillResult = elink::ElegooLink::getInstance().setAutoRefill({params.printerId, false});
            }
        }

        resultCode = parseElegooResult(autoRefillResult.code);
        if (resultCode != PrinterNetworkErrorCode::SUCCESS && resultCode != PrinterNetworkErrorCode::OPERATION_NOT_IMPLEMENTED) {
            return PrinterNetworkResult<bool>(resultCode, false, parseUnknownErrorMsg(resultCode, autoRefillResult.message));
        }

        elinkResult = elink::ElegooLink::getInstance().startPrint(startPrintParams);
        resultCode  = parseElegooResult(elinkResult.code);
        if (resultCode == PrinterNetworkErrorCode::PRINTER_MISSING_BED_LEVELING_DATA && startPrintParams.autoBedLeveling) {
            // missing bed leveling data, send bed leveling data
            startPrintParams.autoBedLeveling = true;
            startPrintParams.bedLevelForce   = true;
            elinkResult                      = elink::ElegooLink::getInstance().startPrint(startPrintParams);
            resultCode                       = parseElegooResult(elinkResult.code);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<bool> ElegooLink::sendPrintFile(const PrinterNetworkParams& params)
{
    CHECK_INITIALIZED(false);
    PrinterNetworkErrorCode           resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::FileUploadResult elinkResult;
  
    try {
        PrinterStatus status;
        // two tries: the first is immediate, the second covers a printer that has not
        // finished settling. Same tolerance as the single try that used to sleep first.
        if (isBusy(params.printerId, status, 2)) {
            return PrinterNetworkResult<bool>(PrinterNetworkErrorCode::PRINTER_BUSY, false);
        }
        elink::FileUploadParams uploadParams;
        uploadParams.printerId       = params.printerId;
        uploadParams.storageLocation = "local";
        uploadParams.localFilePath   = params.filePath;
        uploadParams.fileName        = params.fileName;

        elinkResult = elink::ElegooLink::getInstance().uploadFile(uploadParams, [&](const elink::FileUploadProgressData& progress) -> bool {
            if (params.uploadProgressFn) {
                bool cancel = false;
                params.uploadProgressFn(progress.uploadedBytes, progress.totalBytes, cancel);
                if (cancel) {
                    return false;
                }
            }
            return true;
        });

        resultCode = parseElegooResult(elinkResult.code);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<PrinterMmsGroup> ElegooLink::getPrinterMmsInfo(const std::string& printerId)
{
    CHECK_INITIALIZED(PrinterMmsGroup());
    PrinterMmsGroup              mmsGroup;
    PrinterNetworkErrorCode      resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::GetCanvasStatusResult elinkResult;
    mmsGroup.mmsSystemName = "CANVAS";
    try {
        elinkResult = elink::ElegooLink::getInstance().getCanvasStatus({printerId});
        resultCode  = parseElegooResult(elinkResult.code);
        if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
            if (elinkResult.hasData()) {
                const auto& mmsData   = elinkResult.value();
                mmsGroup.connectNum   = 0;
                mmsGroup.connected    = false;
                mmsGroup.activeMmsId  = std::to_string(mmsData.activeCanvasId);
                mmsGroup.activeTrayId = std::to_string(mmsData.activeTrayId);
                mmsGroup.autoRefill   = mmsData.autoRefill;
                for (const auto& canvas : mmsData.canvases) {
                    PrinterMms mmsInfo;
                    mmsInfo.mmsId     = std::to_string(canvas.canvasId);
                    mmsInfo.connected = canvas.connected;
                    if (canvas.connected) {
                        mmsGroup.connected = true;
                        mmsGroup.connectNum++;
                    }
                    mmsInfo.mmsName  = canvas.name;
                    mmsInfo.mmsModel = canvas.model;
                    for (const auto& tray : canvas.trays) {
                        PrinterMmsTray trayInfo;
                        trayInfo.trayName      = "";
                        trayInfo.trayId        = std::to_string(tray.trayId);
                        trayInfo.mmsId         = std::to_string(canvas.canvasId);
                        trayInfo.vendor        = tray.brand;
                        if(trayInfo.vendor.empty() || trayInfo.vendor == " ") {
                            trayInfo.vendor = "Generic";
                        }
                        trayInfo.filamentType  = tray.filamentType;
                        trayInfo.filamentName  = tray.filamentName;
                        if(trayInfo.filamentName.empty() || trayInfo.filamentName == " ") {
                            trayInfo.filamentName = tray.filamentType;
                        }
                        trayInfo.filamentColor = tray.filamentColor;
                        trayInfo.minNozzleTemp = tray.minNozzleTemp;
                        trayInfo.maxNozzleTemp = tray.maxNozzleTemp;
                        switch (tray.status) {
                        case 0: trayInfo.status = TRAY_STATUS_DISCONNECTED; break;
                        case 1:
                            if (trayInfo.filamentType.empty() || trayInfo.filamentName.empty()) {
                                trayInfo.status = TRAY_STATUS_PRELOADED_UNKNOWN_FILAMENT;
                            } else {
                                trayInfo.status = TRAY_STATUS_PRELOADED;
                            }
                            break;
                        case 2:
                            if (trayInfo.filamentType.empty() || trayInfo.filamentName.empty()) {
                                trayInfo.status = TRAY_STATUS_LOADED_UNKNOWN_FILAMENT;
                            } else {
                                trayInfo.status = TRAY_STATUS_LOADED;
                            }
                            break;
                        default: trayInfo.status = TRAY_STATUS_ERROR; break;
                        }
                        mmsInfo.trayList.push_back(trayInfo);
                    }
                    mmsGroup.mmsList.push_back(mmsInfo);
                }
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }
    return PrinterNetworkResult<PrinterMmsGroup>(resultCode, mmsGroup, parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<PrinterNetworkInfo> ElegooLink::getPrinterAttributes(const std::string& printerId)
{
    CHECK_INITIALIZED(PrinterNetworkInfo());
    PrinterNetworkInfo             printerNetworkInfo;
    PrinterNetworkErrorCode        resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::PrinterAttributesResult elinkResult;
    elink::PrinterAttributesParams params;
    params.printerId = printerId;
    try {
        elinkResult = elink::ElegooLink::getInstance().getPrinterAttributes(params);
        resultCode  = parseElegooResult(elinkResult.code);
        if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
            if (elinkResult.hasData()) {
                const elink::PrinterAttributes& attributes = elinkResult.value();
                printerNetworkInfo                         = convertFromElegooPrinterAttributes(attributes);
            } else {
                resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_INVALID_DATA;
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }
    return PrinterNetworkResult<PrinterNetworkInfo>(resultCode, printerNetworkInfo, parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<PrinterNetworkInfo> ElegooLink::getPrinterStatus(const std::string& printerId)
{
    CHECK_INITIALIZED(PrinterNetworkInfo());
    PrinterNetworkInfo         printerNetworkInfo;
    PrinterNetworkErrorCode    resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::PrinterStatusResult elinkResult;
    elink::PrinterStatusParams params;
    params.printerId = printerId;
    try {
        elinkResult = elink::ElegooLink::getInstance().getPrinterStatus(params);
        resultCode  = parseElegooResult(elinkResult.code);
        if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
            if (elinkResult.hasData()) {
                const auto&      status = elinkResult.value();
                PrinterStatus    s      = parseElegooStatus(status.printerStatus.state, status.printerStatus.subState);
                PrinterPrintTask task;
                task.taskId                      = status.printStatus.taskId;
                task.fileName                    = status.printStatus.fileName;
                task.totalTime                   = status.printStatus.totalTime;
                task.currentTime                 = status.printStatus.currentTime;
                task.estimatedTime               = status.printStatus.estimatedTime;
                task.progress                    = status.printStatus.progress;
                printerNetworkInfo.printerStatus = s;
                for (const auto& exceptionDetail : status.exceptions) {
                    PrinterExceptionDetail detail;
                    detail.code  = exceptionDetail.code;
                    printerNetworkInfo.exceptions.push_back(detail);
                }
                printerNetworkInfo.deviceAssistantStatus = status.deviceAssistantStatus;
                printerNetworkInfo.printTask     = task;

                // PrinterNetworkEvent::getInstance()->printTaskChanged.emit(PrinterPrintTaskEvent(event->status.printerId, task, NETWORK_TYPE_LAN));
            } else {
                resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_INVALID_DATA;
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }
    return PrinterNetworkResult<PrinterNetworkInfo>(resultCode, printerNetworkInfo, parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<std::string> ElegooLink::getPrinterStatusRaw(const std::string& printerId)
{
    CHECK_INITIALIZED(std::string());
    std::string                rawJsonString;
    PrinterNetworkErrorCode    resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::PrinterStatusParams params;
    elink::BizResult<std::string> elinkResult;
    params.printerId = printerId;
    try {
        elinkResult = elink::ElegooLink::getInstance().getPrinterStatusRaw(params);
        resultCode  = parseElegooResult(elinkResult.code);
        if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
            if (elinkResult.hasData()) {
                // Return the raw JSON string directly from the SDK
                rawJsonString = elinkResult.value();
            } else {
                resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_INVALID_DATA;
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": exception: %s") % e.what();
        resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_EXCEPTION;
    }
    return PrinterNetworkResult<std::string>(resultCode, rawJsonString, parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<PrinterPrintFileResponse> ElegooLink::getFileList(const std::string& printerId, int pageNumber, int pageSize)
{
    PrinterPrintFileResponse      printFileResponse;
    std::vector<PrinterPrintFile> printFiles;
    PrinterNetworkErrorCode       resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    printFileResponse.totalFiles             = 0;
    printFileResponse.fileList.clear();

    CHECK_INITIALIZED(printFileResponse);
    elink::GetFileListParams params;
    params.printerId  = printerId;
    params.pageNumber = pageNumber;
    params.pageSize   = pageSize;

    auto elinkResult = elink::ElegooLink::getInstance().getFileList(params);
    resultCode       = parseElegooResult(elinkResult.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        if (elinkResult.hasData()) {
            printFileResponse.totalFiles = elinkResult.value().totalFiles;
            for (const auto& printFile : elinkResult.value().fileList) {
                PrinterPrintFile file;
                file.fileName                = printFile.fileName;
                file.printTime               = printFile.printTime;
                file.layer                   = printFile.layer;
                file.layerHeight             = printFile.layerHeight;
                file.thumbnail               = printFile.thumbnail;
                file.size                    = printFile.size;
                file.createTime              = printFile.createTime;
                file.totalFilamentUsed       = printFile.totalFilamentUsed;
                file.totalFilamentUsedLength = printFile.totalFilamentUsedLength;
                file.totalPrintTimes         = printFile.totalPrintTimes;
                file.lastPrintTime           = printFile.lastPrintTime;
                printFileResponse.fileList.push_back(file);
            }
        }
    }
    return PrinterNetworkResult<PrinterPrintFileResponse>(resultCode, printFileResponse,
                                                          parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<PrinterPrintFileResponse> ElegooLink::getFileDetail(const std::string& printerId, const std::string& fileName)
{
    PrinterNetworkErrorCode  resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    PrinterPrintFileResponse printFileResponse;
    printFileResponse.fileList.clear();
    elink::GetFileDetailParams params;
    params.printerId = printerId;
    params.fileName  = fileName;

    CHECK_INITIALIZED(printFileResponse);

    auto elinkResult = elink::ElegooLink::getInstance().getFileDetail(params);
    resultCode       = parseElegooResult(elinkResult.code);

    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        if (elinkResult.hasData()) {
            const auto&      fileDetail = elinkResult.value();
            PrinterPrintFile printFile;
            printFile.fileName                = fileDetail.fileName;
            printFile.printTime               = fileDetail.printTime;
            printFile.layer                   = fileDetail.layer;
            printFile.layerHeight             = fileDetail.layerHeight;
            printFile.thumbnail               = fileDetail.thumbnail;
            printFile.size                    = fileDetail.size;
            printFile.createTime              = fileDetail.createTime;
            printFile.totalFilamentUsed       = fileDetail.totalFilamentUsed;
            printFile.totalFilamentUsedLength = fileDetail.totalFilamentUsedLength;
            printFile.totalPrintTimes         = fileDetail.totalPrintTimes;
            printFile.lastPrintTime           = fileDetail.lastPrintTime;
            for (const auto& colorMapping : fileDetail.colorMapping) {
                PrintFilamentMmsMapping filamentMmsMapping;

                filamentMmsMapping.filamentColor = colorMapping.color;
                filamentMmsMapping.filamentType  = colorMapping.type;
                // index of the tray in the multi-color printing GCode T command
                filamentMmsMapping.index = colorMapping.t;
                printFile.filamentMmsMappingList.push_back(filamentMmsMapping);
            }
            printFileResponse.fileList.push_back(printFile);
        }
    }
    return PrinterNetworkResult<PrinterPrintFileResponse>(resultCode, printFileResponse,
                                                          parseUnknownErrorMsg(resultCode, elinkResult.message));
}
PrinterNetworkResult<PrinterPrintTaskResponse> ElegooLink::getPrintTaskList(const std::string& printerId, int pageNumber, int pageSize)
{
    PrinterPrintTaskResponse printTaskResponse;
    printTaskResponse.totalTasks = 0;
    printTaskResponse.taskList.clear();

    CHECK_INITIALIZED(printTaskResponse);

    PrinterNetworkErrorCode    resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::PrintTaskListParams params;
    params.printerId  = printerId;
    params.pageNumber = pageNumber;
    params.pageSize   = pageSize;
    auto elinkResult  = elink::ElegooLink::getInstance().getPrintTaskList(params);
    resultCode        = parseElegooResult(elinkResult.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        if (elinkResult.hasData()) {
            printTaskResponse.totalTasks = elinkResult.value().totalTasks;
            for (const auto& printTask : elinkResult.value().taskList) {
                PrinterPrintTask task;
                task.taskId     = printTask.taskId;
                task.thumbnail  = printTask.thumbnail;
                task.taskName   = printTask.taskName;
                task.beginTime  = printTask.beginTime;
                task.endTime    = printTask.endTime;
                task.taskStatus = printTask.taskStatus;
                printTaskResponse.taskList.push_back(task);
            }
        }
    }
    return PrinterNetworkResult<PrinterPrintTaskResponse>(resultCode, printTaskResponse,
                                                          parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<PrinterExceptionResponse> ElegooLink::getExceptionList(const std::string& printerId, int pageNumber, int pageSize)
{
    PrinterExceptionResponse exceptionResponse;
    exceptionResponse.total = 0;
    exceptionResponse.exceptionList.clear();

    CHECK_INITIALIZED(exceptionResponse);

    PrinterNetworkErrorCode       resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::GetExceptionListParams params;
    params.printerId  = printerId;
    params.pageNumber = pageNumber;
    params.pageSize   = pageSize;
    auto elinkResult  = elink::ElegooLink::getInstance().getExceptionList(params);
    resultCode        = parseElegooResult(elinkResult.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        if (elinkResult.hasData()) {
            exceptionResponse.total = elinkResult.value().total;
            for (const auto& exceptionDetail : elinkResult.value().exceptionList) {
                PrinterExceptionDetail detail;
                detail.id    = exceptionDetail.Id;
                detail.refId = exceptionDetail.refId;
                detail.code  = exceptionDetail.code;
                detail.level = exceptionDetail.level;
                detail.time  = exceptionDetail.time;
                exceptionResponse.exceptionList.push_back(detail);
            }
        }
    }
    return PrinterNetworkResult<PrinterExceptionResponse>(resultCode, exceptionResponse,
                                                          parseUnknownErrorMsg(resultCode, elinkResult.message));
}

PrinterNetworkResult<bool> ElegooLink::deletePrintTasks(const std::string& printerId, const std::vector<std::string>& taskIds)
{
    CHECK_INITIALIZED(false);
    elink::DeletePrintTasksResult result = elink::ElegooLink::getInstance().deletePrintTasks(
        elink::DeletePrintTasksParams{printerId, taskIds});
    PrinterNetworkErrorCode resultCode = parseElegooResult(result.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        return PrinterNetworkResult<bool>(resultCode, true);
    }
    return PrinterNetworkResult<bool>(resultCode, true);
}

PrinterNetworkResult<bool> ElegooLink::logout(const UserNetworkInfo& userInfo)
{
    CHECK_INITIALIZED(false);
    elink::VoidResult       result     = elink::ElegooLink::getInstance().logout();
    PrinterNetworkErrorCode resultCode = parseElegooResult(result.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        return PrinterNetworkResult<bool>(resultCode, true);
    }
    return PrinterNetworkResult<bool>(resultCode, false, parseUnknownErrorMsg(resultCode, result.message));
}

PrinterNetworkResult<UserNetworkInfo> ElegooLink::connectToIot(const UserNetworkInfo& userInfo)
{
    CHECK_INITIALIZED(userInfo);
    elink::HttpCredential params;
    params.userId                 = userInfo.userId;
    params.accessToken            = userInfo.token;
    params.refreshToken           = userInfo.refreshToken;
    params.accessTokenExpireTime  = userInfo.accessTokenExpireTime;
    params.refreshTokenExpireTime = userInfo.refreshTokenExpireTime;

    UserNetworkInfo userInfoRet = userInfo;

    elink::VoidResult       setHttpCredentialResult = elink::ElegooLink::getInstance().setHttpCredential(params);
    PrinterNetworkErrorCode resultCode              = parseElegooResult(setHttpCredentialResult.code);

    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        elink::GetUserInfoParams params;
        elink::GetUserInfoResult userInfoResult = elink::ElegooLink::getInstance().getUserInfo(params);
        resultCode                              = parseElegooResult(userInfoResult.code);
        if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
            if (userInfoResult.hasData()) {
                const auto& userInfoData = userInfoResult.value();
                userInfoRet.userId       = userInfoData.userId;
                userInfoRet.phone        = userInfoData.phone;
                userInfoRet.email        = userInfoData.email;
                userInfoRet.nickname     = userInfoData.nickName;
                userInfoRet.avatar       = userInfoData.avatar;
            } else {
                resultCode = PrinterNetworkErrorCode::INVALID_USERNAME_OR_PASSWORD;
            }
        }
    }
    return PrinterNetworkResult<UserNetworkInfo>(resultCode, userInfoRet, parseUnknownErrorMsg(resultCode, setHttpCredentialResult.message));
}

PrinterNetworkResult<bool> ElegooLink::updatePrinterName(const std::string& printerId, const std::string& printerName)
{
    CHECK_INITIALIZED(false);
    elink::VoidResult              result;
    elink::UpdatePrinterNameParams params;
    params.printerId                   = printerId;
    params.printerName                 = printerName;
    result                             = elink::ElegooLink::getInstance().updatePrinterName(params);
    if(result.code == elink::ELINK_ERROR_CODE::OPERATION_NOT_IMPLEMENTED) {
        result.code = elink::ELINK_ERROR_CODE::SUCCESS;
    }
    PrinterNetworkErrorCode resultCode = parseElegooResult(result.code);
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, result.message));
}

PrinterNetworkResult<bool> ElegooLink::cancelBindPrinter(const std::string& serialNumber)
{
    CHECK_INITIALIZED(false);
    PrinterNetworkErrorCode resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    elink::VoidResult              result;
    elink::CancelBindPrinterParams params;
    params.serialNumber = serialNumber;
    result = elink::ElegooLink::getInstance().cancelBindPrinter(params);
    resultCode = parseElegooResult(result.code);
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, result.message));
}   
PrinterNetworkResult<UserNetworkInfo> ElegooLink::getRtcToken()
{
    CHECK_INITIALIZED(UserNetworkInfo());
    UserNetworkInfo          userInfo;
    elink::GetRtcTokenResult result     = elink::ElegooLink::getInstance().getRtcToken();
    PrinterNetworkErrorCode  resultCode = parseElegooResult(result.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        if (result.hasData()) {
            const auto& rtcTokenData    = result.value();
            userInfo.userId             = rtcTokenData.userId;
            userInfo.rtcToken           = rtcTokenData.rtcToken;
            userInfo.rtcTokenExpireTime = rtcTokenData.rtcTokenExpireTime;
        }
    }
    return PrinterNetworkResult<UserNetworkInfo>(resultCode, userInfo, parseUnknownErrorMsg(resultCode, result.message));
}

PrinterNetworkResult<UserNetworkInfo> ElegooLink::refreshToken(const UserNetworkInfo& userInfo)
{
    CHECK_INITIALIZED(userInfo);
    UserNetworkInfo       userInfoRet = userInfo;
    elink::HttpCredential params;
    params.userId                 = userInfo.userId;
    params.refreshToken           = userInfo.refreshToken;
    params.accessToken            = userInfo.token;
    params.accessTokenExpireTime  = userInfo.accessTokenExpireTime;
    params.refreshTokenExpireTime = userInfo.refreshTokenExpireTime;

    elink::BizResult<elink::HttpCredential> httpCredentialResult = elink::ElegooLink::getInstance().refreshHttpCredential(params);
    PrinterNetworkErrorCode                 resultCode           = parseElegooResult(httpCredentialResult.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        if (httpCredentialResult.hasData() && userInfo.userId == httpCredentialResult.value().userId) {
            elink::HttpCredential httpCredential = httpCredentialResult.value();
            userInfoRet.token                    = httpCredential.accessToken;
            userInfoRet.refreshToken             = httpCredential.refreshToken;
            userInfoRet.accessTokenExpireTime    = httpCredential.accessTokenExpireTime;
            userInfoRet.refreshTokenExpireTime   = httpCredential.refreshTokenExpireTime;
        } else {
            resultCode = PrinterNetworkErrorCode::PRINTER_NETWORK_INVALID_DATA;
        }
    }

    return PrinterNetworkResult<UserNetworkInfo>(resultCode, userInfoRet, parseUnknownErrorMsg(resultCode, httpCredentialResult.message));
}

PrinterNetworkResult<bool> ElegooLink::sendRtmMessage(const std::string& printerId, const std::string& message)
{
    CHECK_INITIALIZED(false);
    elink::SendRtmMessageParams params;
    params.printerId                   = printerId;
    params.message                     = message;
    elink::VoidResult       result     = elink::ElegooLink::getInstance().sendRtmMessage(params);
    PrinterNetworkErrorCode resultCode = parseElegooResult(result.code);
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, result.message));
}

PrinterNetworkResult<std::vector<LicenseExpiredDevice>> ElegooLink::getLicenseExpiredDevices()
{
    CHECK_INITIALIZED(std::vector<LicenseExpiredDevice>());

    elink::GetLicenseExpiredDevicesResult result = elink::ElegooLink::getInstance().getLicenseExpiredDevices();
    PrinterNetworkErrorCode resultCode = parseElegooResult(result.code);
    
    std::vector<LicenseExpiredDevice> devices;
    if (resultCode == PrinterNetworkErrorCode::SUCCESS && result.hasData()) {
        for (const auto& device : result.value().devices) {
            LicenseExpiredDevice licenseDevice;
            licenseDevice.serialNumber = device.serialNumber;
            licenseDevice.status = device.status;
            devices.push_back(licenseDevice);
        }
    }
    
    return PrinterNetworkResult<std::vector<LicenseExpiredDevice>>(resultCode, devices,
                                      parseUnknownErrorMsg(resultCode, result.message));
}

PrinterNetworkResult<bool> ElegooLink::renewLicense(const std::string& serialNumber)
{
    CHECK_INITIALIZED(false);
    
    elink::RenewLicenseParams params;
    params.serialNumber = serialNumber;
    elink::RenewLicenseResult result = elink::ElegooLink::getInstance().renewLicense(params);
    PrinterNetworkErrorCode resultCode = parseElegooResult(result.code);
    
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, result.message));
}

PrinterNetworkResult<bool> ElegooLink::refreshPrinterStatus(const std::string& printerId)
{
    CHECK_INITIALIZED(false);
    
    elink::PrinterStatusParams params;
    params.printerId = printerId;
    elink::VoidResult result = elink::ElegooLink::getInstance().refreshPrinterStatus(params);
    PrinterNetworkErrorCode resultCode = parseElegooResult(result.code);
    
    return PrinterNetworkResult<bool>(resultCode, resultCode == PrinterNetworkErrorCode::SUCCESS,
                                      parseUnknownErrorMsg(resultCode, result.message));
}

PrinterNetworkResult<std::vector<PrinterNetworkInfo>> ElegooLink::getUserBoundPrinters()
{
    CHECK_INITIALIZED(std::vector<PrinterNetworkInfo>());

    PrinterNetworkErrorCode         resultCode = PrinterNetworkErrorCode::UNKNOWN_ERROR;
    std::vector<PrinterNetworkInfo> printers;

    elink::GetPrinterListResult elinkResult;
    elinkResult = elink::ElegooLink::getInstance().getPrinters();
    resultCode  = parseElegooResult(elinkResult.code);
    if (resultCode == PrinterNetworkErrorCode::SUCCESS) {
        if (elinkResult.hasData()) {
            for (const auto& printer : elinkResult.value().printers) {
                if(printer.networkMode == elink::NetworkMode::LAN) {
                    continue;
                }
                printers.push_back(convertFromElegooPrinterInfo(printer));
            }
        }
    }
    return PrinterNetworkResult<std::vector<PrinterNetworkInfo>>(resultCode, printers,
                                                                 parseUnknownErrorMsg(resultCode, elinkResult.message));
}

} // namespace Slic3r
