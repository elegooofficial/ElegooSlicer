#pragma once


#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include <wx/dialog.h>
#include <wx/webview.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/event.h>
#include <wx/dialog.h>

#include <nlohmann/json.hpp>
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrinterNetworkInfo.hpp"
#include <slic3r/Utils/WebviewIPCManager.h>

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

namespace webviewIpc {
    class WebviewIPCManager;
}

namespace Slic3r { namespace GUI {
class PrintSendDialogEx : public GUI::DPIDialog
{
public:
    PrintSendDialogEx(Plater* plater, int printPlateIdx, const boost::filesystem::path& path);

    ~PrintSendDialogEx();

    void init() ;
    boost::filesystem::path filename() const {
        return into_path(mModelName);
    }

    virtual void EndModal(int ret) override;

    // the per-job key/value block PrintHostUpload carries
    using ExtendedInfo = std::map<std::string, std::string>;

    ExtendedInfo getExtendedInfo() const ;
    // One extended-info block per additional printer: same G-code, own id and mapping.
    // by value under the lock, for the same reason getExtendedInfo() takes it: a handler
    // can still be running when these are read
    std::vector<ExtendedInfo> getAdditionalExtendedInfo() const;
    std::vector<std::pair<std::string, std::string>> getDroppedPrinters() const;
    PrintHostPostUploadAction getPostAction() const;
    bool getSwitchToDeviceTab() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void OnCloseWindow(wxCloseEvent& event);

    BedType getCurrentBedType() const;
private:
    void setupIPCHandlers();
    IPCResult getPrinterList();
    IPCResult preparePrintTask(const std::string &printerId);
    IPCResult getPrinterMmsInfo(const std::string &printerId);
    IPCResult onPrint(const nlohmann::json &printInfo);
    IPCResult getAdditionalPrinterMmsInfo(const std::string &printerId); // tray state + suggested mapping for any printer
    // builds the extended info for one additional printer, or an error if it cannot run the job
    PrinterNetworkResult<ExtendedInfo> resolveAdditionalPrinter(const nlohmann::json &printerEntry,
                                                                bool uploadAndPrint);
    void onCancel();
    std::string getCurrentProjectName(); 
    BedType appBedType() const;
    void    refresh();

    // WebviewIPCManager dispatches every handler, sync and async alike, onto a shared
    // worker pool, so handlers can run concurrently over the members below. Every member
    // function a handler calls takes this mutex; handlers that only queue GUI work do not.
    mutable std::mutex mIpcMutex;

    // Liveness token for work queued from the IPC pool onto the GUI thread: reset in the
    // destructor, so an expired weak_ptr means this dialog is gone. A window id can be
    // recycled once wxIdManager's counter is exhausted, and wxWeakRef's tracker list is
    // not thread-safe; a refcount is neither.
    std::shared_ptr<char> mAlive = std::make_shared<char>();

    // Queues fn onto the GUI thread, dropping it if the dialog dies first. Handlers run on
    // the IPC pool and can outlive the dialog, so every queued call goes through this.
    template<typename Fn> void callAfterIfAlive(Fn&& fn)
    {
        std::weak_ptr<char> alive = mAlive;
        wxGetApp().CallAfter([this, alive, fn = std::forward<Fn>(fn)]() {
            if (!alive.expired()) {
                fn();
            }
        });
    }

    wxWebView* mBrowser;
    std::unique_ptr<webviewIpc::WebviewIPCManager> mIpc;
    Plater*  mPlater{ nullptr };
    int mPrintPlateIdx;

    bool    mTimeLapse{false};
    bool    mHeatedBedLeveling;
    BedType mBedType;
    bool    mAutoRefill;
    PrintHostPostUploadAction mPostUploadAction;
    bool mSwitchToDeviceTab;
    wxString mModelName;
    boost::filesystem::path mPath;
    std::string mSelectedPrinterId;
    std::string mProjectName;
    std::vector<PrintFilamentMmsMapping> mPrintFilamentList;
    bool mHasMms;
    PrinterMmsGroup mMmsGroup;
    std::vector<ExtendedInfo> mAdditionalExtendedInfo;
    std::vector<std::pair<std::string, std::string>> mDroppedPrinters; // (printer name, reason)
   
};
}} // namespace Slic3r::GUI 
