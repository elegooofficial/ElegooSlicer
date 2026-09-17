#include "PrintHost.hpp"

#include <vector>
#include <thread>
#include <exception>
#include <boost/optional.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>

#include <wx/string.h>
#include <wx/app.h>
#include <wx/arrstr.h>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Channel.hpp"
#include "OctoPrint.hpp"
#include "Duet.hpp"
#include "FlashAir.hpp"
#include "AstroBox.hpp"
#include "Repetier.hpp"
#include "MKS.hpp"
#include "ESP3D.hpp"
#include "CrealityPrint.hpp"
#include "../GUI/PrintHostDialogs.hpp"
#include "../GUI/MainFrame.hpp"
#include "Obico.hpp"
#include "Flashforge.hpp"
#include "SimplyPrint.hpp"
#include "Elegoo/PrinterManager.hpp"
#include "Elegoo/PrinterUploadManager.hpp"
#include "Elegoo/PrinterMmsManager.hpp"
#include "libslic3r/PrinterNetworkInfo.hpp"
#include "3DPrinterOS.hpp"
#include "Moonraker.hpp"

namespace fs = boost::filesystem;
using boost::optional;
using Slic3r::GUI::PrintHostQueueDialog;

namespace Slic3r {


PrintHost::~PrintHost() {}

PrintHost* PrintHost::get_print_host(DynamicPrintConfig *config)
{
    PrinterTechnology tech = ptFFF;

    {
        const auto opt = config->option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
        if (opt != nullptr) {
            tech = opt->value;
        }
    }

    if (tech == ptFFF) {
        const auto opt = config->option<ConfigOptionEnum<PrintHostType>>("host_type");
        const auto host_type = opt != nullptr ? opt->value : htOctoPrint;

        switch (host_type) {
            case htOctoPrint: return new OctoPrint(config);
            case htDuet:      return new Duet(config);
            case htFlashAir:  return new FlashAir(config);
            case htAstroBox:  return new AstroBox(config);
            case htRepetier:  return new Repetier(config);
            case htPrusaLink: return new PrusaLink(config);
            case htPrusaConnect: return new PrusaConnect(config);
            case htMKS:       return new MKS(config);
            case htESP3D:       return new ESP3D(config);
            case htCrealityPrint:    return new CrealityPrint(config);
            case htObico:     return new Obico(config);
            case htFlashforge: return new Flashforge(config);
            case htSimplyPrint: return new SimplyPrint(config);
            case htElegooLink: return new OctoPrint(config);
            case ht3DPrinterOS: return new C3DPrinterOS(config);
            case htMoonraker: return new Moonraker(config);
            default:          return nullptr;
        }
    } else {
        return new SL1Host(config);
    }
}
PrintHostType PrintHost::get_print_host_type(const DynamicPrintConfig &config)
{
    const auto opt = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    const auto host_type = opt != nullptr ? opt->value : htOctoPrint;
    return host_type;
}
bool PrintHost::support_device_list_management(const DynamicPrintConfig &config)
{
    PrintHostType host_type = PrintHost::get_print_host_type(config);
    std::vector<PrintHostType> support_device_list_management_host_types = {htElegooLink};
    return std::find(support_device_list_management_host_types.begin(), support_device_list_management_host_types.end(), host_type) != support_device_list_management_host_types.end();
}

bool PrintHost::support_mms(const DynamicPrintConfig &config)
{
    const ConfigOptionBool* support_mms = config.option<ConfigOptionBool>("support_mms");
    return support_mms != nullptr && support_mms->value;
}

PrintHostType PrintHost::get_print_host_type(const std::string &host_type_str)
{
    if(host_type_str == "ElegooLink") {
        return htElegooLink;
    } else if(host_type_str == "OctoPrint") {
        return htOctoPrint;
    } else if(host_type_str == "PrusaLink") {
        return htPrusaLink;
    } else if(host_type_str == "PrusaConnect") {
        return htPrusaConnect;
    } else if(host_type_str == "Duet") {
        return htDuet;
    } else if(host_type_str == "FlashAir") {
        return htFlashAir;
    } else if(host_type_str == "AstroBox") {
        return htAstroBox;
    } else if(host_type_str == "Repetier") {
        return htRepetier;
    } else if(host_type_str == "MKS") {
        return htMKS;
    } else if(host_type_str == "ESP3D") {
        return htESP3D;
    } else if(host_type_str == "CrealityPrint") {
        return htCrealityPrint;
    } else if(host_type_str == "Obico") {
        return htObico;
    } else if(host_type_str == "Flashforge") {
        return htFlashforge;
    } else if(host_type_str == "SimplyPrint") {
        return htSimplyPrint;
    }
    return htOctoPrint;
}
std::string PrintHost::get_print_host_type_str(const PrintHostType host_type)
{
    std::string host_type_str;
    switch (host_type) {
        case htElegooLink: host_type_str = "ElegooLink"; break;
        case htOctoPrint: host_type_str = "OctoPrint"; break;
        case htPrusaLink: host_type_str = "PrusaLink"; break;
        case htPrusaConnect: host_type_str = "PrusaConnect"; break;
        case htDuet: host_type_str = "Duet"; break;
        case htFlashAir: host_type_str = "FlashAir"; break;
        case htAstroBox: host_type_str = "AstroBox"; break;
        case htRepetier: host_type_str = "Repetier"; break;
        case htMKS: host_type_str = "MKS"; break;
        case htESP3D: host_type_str = "ESP3D"; break;
        case htCrealityPrint: host_type_str = "CrealityPrint"; break;
        case htObico: host_type_str = "Obico"; break;
        case htFlashforge: host_type_str = "Flashforge"; break;
        case htSimplyPrint: host_type_str = "SimplyPrint"; break;
        default: host_type_str = ""; break;
    }
    return host_type_str;
}

std::string PrintHost::get_print_host_webui(DynamicPrintConfig* config)
{
    if (config == nullptr)
        return {};

    std::string webui_url;
    const auto* host_type_opt = config->option<ConfigOptionEnum<PrintHostType>>("host_type");
    const auto  host_type     = host_type_opt != nullptr ? host_type_opt->value : htOctoPrint;

    switch (host_type) {
    // case htElegooLink: {
    //     webui_url = ElegooLink::get_print_host_webui(config);
    //     break;
    // }
    default: break;
    }

    if (webui_url.empty()) {
        webui_url = config->opt_string("print_host_webui");
        if (webui_url.empty())
            webui_url = config->opt_string("print_host");
        if (webui_url.empty())
            return webui_url;
    }

    const bool has_http_scheme = boost::algorithm::istarts_with(webui_url, "http");
    const bool has_file_scheme = boost::algorithm::istarts_with(webui_url, "file:");

    if (!has_http_scheme && !has_file_scheme)
        webui_url = "http://" + webui_url;

    return webui_url;
}

wxString PrintHost::format_error(const std::string &body, const std::string &error, unsigned status) const
{
    if (status != 0) {
        auto wxbody = wxString::FromUTF8(body.data());
        return wxString::Format("HTTP %u: %s", status, wxbody);
    } else {
        if (error.find("curl:Timeout was reached") != std::string::npos) {
            return _L("Connection timed out. Please check if the printer and computer network are functioning properly, and confirm that they are on the same network.");
        }else if(error.find("curl:Couldn't resolve host name")!= std::string::npos){
            return _L("The Hostname/IP/URL could not be parsed, please check it and try again.");
        } else if (error.find("Connection was reset") != std::string::npos){
            return _L("File/data transfer interrupted. Please check the printer and network, then try it again.");
        }
        else {
            return wxString::FromUTF8(error.data());
        }
    }
}


struct PrintHostJobQueue::priv
{
    // XXX: comment on how bg thread works

    PrintHostJobQueue *q;

    Channel<PrintHostJob> channel_jobs;
    Channel<size_t> channel_cancels;
    size_t job_id = 0;
    int prev_progress = -1;
    fs::path source_to_remove;

    std::thread bg_thread;
    bool bg_exit = false;

    PrintHostQueueDialog *queue_dialog;

    priv(PrintHostJobQueue *q) : q(q) {}

    void emit_progress(int progress);
    void emit_error(wxString error);
    void emit_cancel(size_t id);
    void emit_info(wxString tag, wxString status);
    void start_bg_thread();
    void stop_bg_thread();
    void bg_thread_main();
    void progress_fn(Http::Progress progress, bool &cancel);
    void error_fn(wxString error);
    void info_fn(wxString tag, wxString status);
    void remove_source(const fs::path &path);
    void remove_source();
    void perform_job(PrintHostJob the_job);
};

PrintHostJobQueue::PrintHostJobQueue(PrintHostQueueDialog *queue_dialog)
    : p(new priv(this))
{
    p->queue_dialog = queue_dialog;
}

PrintHostJobQueue::~PrintHostJobQueue()
{
    if (p) { p->stop_bg_thread(); }
}

void PrintHostJobQueue::priv::emit_progress(int progress)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_PROGRESS, queue_dialog->GetId(), job_id, progress);
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::emit_error(wxString error)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_ERROR, queue_dialog->GetId(), job_id, std::move(error));
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::emit_info(wxString tag, wxString status)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_INFO, queue_dialog->GetId(), job_id, std::move(tag), std::move(status));
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::emit_cancel(size_t id)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_CANCEL, queue_dialog->GetId(), id);
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::start_bg_thread()
{
    if (bg_thread.joinable()) { return; }

    std::shared_ptr<priv> p2 = q->p;
    bg_thread = std::thread([p2]() {
        p2->bg_thread_main();
    });
}

void PrintHostJobQueue::priv::stop_bg_thread()
{
    if (bg_thread.joinable()) {
        bg_exit = true;
        channel_jobs.push(PrintHostJob()); // Push an empty job to wake up bg_thread in case it's sleeping
        bg_thread.detach();                // Let the background thread go, it should exit on its own
    }
}

void PrintHostJobQueue::priv::bg_thread_main()
{
    // bg thread entry point

    try {
        // Pick up jobs from the job channel:
        while (! bg_exit) {
            auto job = channel_jobs.pop();   // Sleeps in a cond var if there are no jobs
            if (job.empty()) {
                // This happens when the thread is being stopped
                break;
            }

            source_to_remove = job.upload_data.source_path;

            BOOST_LOG_TRIVIAL(debug) << boost::format("PrintHostJobQueue/bg_thread: Received job: [%1%]: `%2%` -> `%3%`, cancelled: %4%")
                % job_id
                % job.upload_data.upload_path
                % job.printhost->get_host()
                % job.cancelled;

            if (! job.cancelled) {
                // do not let one failing job take the queue thread down with the jobs behind it
                try {
                    perform_job(std::move(job));
                } catch (const std::exception &e) {
                    emit_error(e.what());
                }
            }

            remove_source();
            job_id++;
        }
    } catch (const std::exception &e) {
        emit_error(e.what());
    }

    // Cleanup leftover files, if any
    remove_source();
    auto jobs = channel_jobs.lock_rw();
    for (const PrintHostJob &job : *jobs) {
        remove_source(job.upload_data.source_path);
    }
}

void PrintHostJobQueue::priv::progress_fn(Http::Progress progress, bool &cancel)
{
    if (cancel) {
        // When cancel is true from the start, Http indicates request has been cancelled
        emit_cancel(job_id);
        return;
    }

    if (bg_exit) {
        cancel = true;
        return;
    }

    if (channel_cancels.size_hint() > 0) {
        // Lock both queues
        auto cancels = channel_cancels.lock_rw();
        auto jobs = channel_jobs.lock_rw();

        for (size_t cancel_id : *cancels) {
            if (cancel_id == job_id) {
                cancel = true;
            } else if (cancel_id > job_id) {
                const size_t idx = cancel_id - job_id - 1;
                if (idx < jobs->size()) {
                    jobs->at(idx).cancelled = true;
                    BOOST_LOG_TRIVIAL(debug) << boost::format("PrintHostJobQueue: Job id %1% cancelled") % cancel_id;
                    emit_cancel(cancel_id);
                }
            }
        }

        cancels->clear();
    }

    if (! cancel) {
        int gui_progress = progress.ultotal > 0 ? 100*progress.ulnow / progress.ultotal : 0;
        if (gui_progress != prev_progress) {
            emit_progress(gui_progress);
            prev_progress = gui_progress;
        }
    }
}

void PrintHostJobQueue::priv::error_fn(wxString error)
{
    // check if transfer was not canceled before error occured - than do not show the error
    bool do_emit_err = true;
    if (channel_cancels.size_hint() > 0) {
        // Lock both queues
        auto cancels = channel_cancels.lock_rw();
        auto jobs = channel_jobs.lock_rw();

        for (size_t cancel_id : *cancels) {
            if (cancel_id == job_id) {
                do_emit_err = false;
                emit_cancel(job_id);
            }
            else if (cancel_id > job_id) {
                const size_t idx = cancel_id - job_id - 1;
                if (idx < jobs->size()) {
                    jobs->at(idx).cancelled = true;
                    BOOST_LOG_TRIVIAL(debug) << boost::format("PrintHostJobQueue: Job id %1% cancelled") % cancel_id;
                    emit_cancel(cancel_id);
                }
            }
        }
        cancels->clear();
    }
    if (do_emit_err)
        emit_error(std::move(error));
}

void PrintHostJobQueue::priv::info_fn(wxString tag, wxString status)
{
    emit_info(tag, status);
}

void PrintHostJobQueue::priv::remove_source(const fs::path &path)
{
    if (! path.empty()) {
        boost::system::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << boost::format("PrintHostJobQueue: Error removing file `%1%`: %2%") % path % ec;
        }
    }
}

void PrintHostJobQueue::priv::remove_source()
{
    remove_source(source_to_remove);
    source_to_remove.clear();
}

void PrintHostJobQueue::priv::perform_job(PrintHostJob the_job)
{
    emit_progress(0);   // Indicate the upload is starting
    bool success = false;
    std::string selectedPrinterId = "";
    DynamicPrintConfig cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    if (PrintHost::support_device_list_management(cfg)) {
        PrinterNetworkParams params;
        params.filePath = the_job.upload_data.source_path.string();
        params.fileName = the_job.upload_data.upload_path.string();
        params.bedType = 0;
        if(the_job.upload_data.extended_info.find("bedType") != the_job.upload_data.extended_info.end()) {
            int bedType = std::stoi(the_job.upload_data.extended_info["bedType"]);
            if(bedType == BedType::btPC) {
                params.bedType = 1;
            }
        }
        if(the_job.upload_data.extended_info.find("timeLapse") != the_job.upload_data.extended_info.end()) {
            params.timeLapse = the_job.upload_data.extended_info["timeLapse"] == "true";
        }
        if(the_job.upload_data.extended_info.find("heatedBedLeveling") != the_job.upload_data.extended_info.end()) {
            params.heatedBedLeveling = the_job.upload_data.extended_info["heatedBedLeveling"] == "true";
        }
        if(the_job.upload_data.extended_info.find("autoRefill") != the_job.upload_data.extended_info.end()) {
            params.autoRefill = the_job.upload_data.extended_info["autoRefill"] == "true";
        }
        if(the_job.upload_data.extended_info.find("hasMms") != the_job.upload_data.extended_info.end()) {
            params.hasMms = the_job.upload_data.extended_info["hasMms"] == "true";
        }

        if(the_job.upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
            params.uploadAndStartPrint = true;
        }
        if(the_job.upload_data.extended_info.find("selectedPrinterId") != the_job.upload_data.extended_info.end()) {
            params.printerId = the_job.upload_data.extended_info["selectedPrinterId"];
            selectedPrinterId = params.printerId;
        }
        if(the_job.upload_data.extended_info.find("filamentAmsMapping") != the_job.upload_data.extended_info.end()) {
            nlohmann::json filamentAmsMapping = nlohmann::json::parse(the_job.upload_data.extended_info["filamentAmsMapping"]);
            for(auto& filament : filamentAmsMapping) {
                PrintFilamentMmsMapping printFilamentMmsMapping = convertJsonToPrintFilamentMmsMapping(filament);
                params.filamentMmsMappingList.push_back(printFilamentMmsMapping);
            }
        }
        params.uploadProgressFn = [this](uint64_t uploadedBytes, uint64_t totalBytes, bool& cancel) { 
            Http::Progress progress(totalBytes, uploadedBytes, totalBytes, uploadedBytes, "");
            this->progress_fn(std::move(progress), cancel);
        };
        params.errorFn = [this](const std::string& errorMsg) { 
            this->error_fn(wxString::FromUTF8(errorMsg)); 
        };
        auto result = PrinterUploadManager::getInstance()->upload(params);
        success = result.isSuccess();
    } else {
        success = the_job.printhost->upload(
            std::move(the_job.upload_data),
            [this](Http::Progress progress, bool& cancel) { this->progress_fn(std::move(progress), cancel); },
            [this](wxString error) { this->error_fn(std::move(error)); },
            [this](wxString tag, wxString host) { this->info_fn(std::move(tag), std::move(host)); });
    }
    if (success) {
        emit_progress(100);
        if (the_job.switch_to_device_tab) {
            const auto mainframe = GUI::wxGetApp().mainframe;
            mainframe->request_select_tab(MainFrame::TabPosition::tpMonitor, selectedPrinterId);
        }
    }
}

void PrintHostJobQueue::enqueue(PrintHostJob job)
{
    p->start_bg_thread();
    p->queue_dialog->append_job(job);
    p->channel_jobs.push(std::move(job));
}

void PrintHostJobQueue::cancel(size_t id)
{
    p->channel_cancels.push(id);
}

}
