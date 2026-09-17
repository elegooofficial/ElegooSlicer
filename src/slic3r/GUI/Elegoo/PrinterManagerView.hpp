#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <functional>
#include <atomic>
#include <memory>
#include "ElegooPrinterWebView.hpp"
#include <wx/webview.h>
#include <wx/aui/aui.h>
#include <wx/colour.h>
#include <nlohmann/json.hpp>
#include "slic3r/GUI/Plater.hpp"
#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "slic3r/Utils/WebviewIPCManager.h"
#include "libslic3r/PrinterNetworkInfo.hpp"

namespace Slic3r { namespace GUI {

class PrinterManagerView : public wxPanel
{
public:
    PrinterManagerView(wxWindow *parent);
    virtual ~PrinterManagerView();
    void onClose(wxCloseEvent& evt);
    // focusTab=false opens (or re-uses) the printer's tab without selecting it. A
    // multi-printer send opens a tab per target as each upload completes; without this
    // every completion would steal focus, leaving the operator on whichever machine
    // happened to finish last rather than the one they chose.
    void openPrinterTab(const std::string& printerId, bool saveState = true, bool openDeviceAssistant = false,
                        bool focusTab = true);
    void refreshUserInfo();
    void msw_rescale();
    bool Show(bool show = true) override;
    
    // Initialize WebView after window is shown (fixes macOS multi-display rendering issue)
    void initializeWebView();

private:
    bool createMainBrowser();
    void resetMainBrowser();
    void setupIPCHandlers();
    void onClosePrinterTab(wxAuiNotebookEvent& event);
    void onTabBeginDrag(wxAuiNotebookEvent& event);
    void onTabDragMotion(wxAuiNotebookEvent& event);
    void onTabEndDrag(wxAuiNotebookEvent& event);
    void onTabChanged(wxAuiNotebookEvent& event);

    IPCResult getPrinterList();
    IPCResult discoverPrinter();
    IPCResult getPrinterModelList();
    IPCResult addPrinter(const nlohmann::json& printer);
    IPCResult addPhysicalPrinter(const nlohmann::json& printer);
    IPCResult cancelBindPrinter(const nlohmann::json& printer);
    IPCResult updatePrinterName(const std::string& printerId, const std::string& printerName);
    IPCResult updatePrinterHost(const std::string& printerId, const std::string& host);
    IPCResult updatePhysicalPrinter(const std::string& printerId, const nlohmann::json& printer);
    IPCResult deletePrinter(const std::string& printerId);
    IPCResult browseCAFile();
    IPCResult handleReady();
    IPCResult handleCheckLoginStatus();

    // Tab persistence methods
    void saveTabState();
    void loadTabState();
    
    void closeInvalidPrinterTab(std::vector<PrinterNetworkInfo>& printerList);
    
    ElegooPrinterWebView* findPrinterView(const std::string& printerId);
    void insertPrinterView(const std::string& printerId, ElegooPrinterWebView* view);
    bool removePrinterView(const std::string& printerId);
    ElegooPrinterWebView* removePrinterViewByWindow(wxWindow* win);
    void forEachPrinterView(std::function<void(const std::string&, ElegooPrinterWebView*)> callback);
    
private:
    wxAuiNotebook* mTabBar;
    wxWebView* mBrowser;
    std::unique_ptr<webviewIpc::WebviewIPCManager> mIpc;
    std::mutex mPrinterViewsMutex;
    std::map<std::string, ElegooPrinterWebView*> mPrinterViews;
    bool mFirstTabClicked{false};
    std::mutex mUserInfoMutex; // Mutex to protect user info
    UserNetworkInfo mRefreshUserInfo; // User info
    std::atomic<bool> mIsReady{false};
    std::atomic<bool> mWebViewInitialized{false};
    bool mResetWebViewOnShow{false};
    std::mutex mPrinterSnapshotMutex;
    std::size_t mLastObservedPrinterCount{0};
    bool mHasObservedPrinterCount{false};

    uint64_t mConnectStatusChangedHandlerId{0};
    uint64_t mEventRawChangedHandlerId{0};
    uint64_t mRtcTokenChangedHandlerId{0};
    uint64_t mRtmMessageChangedHandlerId{0};
};
}} // namespace Slic3r::GUI 
