#include "Shared.hpp"
#include "Fluent.hpp"
#include "REWParser.hpp"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <commdlg.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <memory>
#include <atomic>

using namespace macsound;
using namespace macsound::win;
namespace {
constexpr UINT deviceMessage=WM_APP+1,trayMessage=WM_APP+2;
constexpr int renderControl=100,captureControl=101,leftControl=102,rightControl=103,
    eqControl=104,aecControl=105,installControl=106,removeControl=107,statusControl=108;
struct Device {std::wstring id,name;};
HWND window=nullptr,renderCombo=nullptr,captureCombo=nullptr,eqCheck=nullptr,aecCheck=nullptr,statusText=nullptr;
SharedFile shared;
Configuration configuration;
LONG revision=0;
std::vector<Device> outputs,inputs;
std::wstring notice,lastStatus;
int pageWidth=900,scrollOffset=0;
std::wstring leftSummary=L"설치 시 L.txt를 가져옵니다",rightSummary=L"설치 시 R.txt를 가져옵니다";
IMMDeviceEnumerator* enumerator=nullptr;
NOTIFYICONDATAW tray{};
std::wstring wide(const std::string& text) {
    const auto n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring out(n,L' ');MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n);return out;
}
void error(const std::wstring& text) {MessageBoxW(window,text.c_str(),L"MacTools",MB_OK|MB_ICONERROR);}
std::filesystem::path executableDirectory() {wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);return std::filesystem::path(path).parent_path();}
std::vector<Device> enumerate(EDataFlow flow) {
    std::vector<Device> result;IMMDeviceCollection* collection=nullptr;
    if(!enumerator||FAILED(enumerator->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,&collection)))return result;
    UINT n=0;collection->GetCount(&n);
    for(UINT i=0;i<n;++i) {
        IMMDevice* device=nullptr;LPWSTR id=nullptr;IPropertyStore* properties=nullptr;
        if(FAILED(collection->Item(i,&device)))continue;
        if(SUCCEEDED(device->GetId(&id))&&SUCCEEDED(device->OpenPropertyStore(STGM_READ,&properties))) {
            PROPVARIANT name;PropVariantInit(&name);
            if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&name))&&name.vt==VT_LPWSTR)result.push_back({id,name.pwszVal});
            PropVariantClear(&name);
        }
        if(properties)properties->Release();CoTaskMemFree(id);device->Release();
    }
    collection->Release();return result;
}
bool containsDevice(const std::vector<Device>& devices,const wchar_t* id) {
    for(const auto& d:devices)if(d.id==id)return true;return false;
}
void fillCombo(HWND combo,const std::vector<Device>& devices,const wchar_t* selected) {
    SendMessageW(combo,CB_RESETCONTENT,0,0);int chosen=-1;
    for(std::size_t i=0;i<devices.size();++i) {
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(devices[i].name.c_str()));
        if(devices[i].id==selected)chosen=static_cast<int>(i);
    }
    if(chosen<0&&*selected) {
        chosen=static_cast<int>(devices.size());
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"선택 장치 연결 대기"));
    }
    SendMessageW(combo,CB_SETCURSEL,chosen,0);
}
void refreshDevices() {
    outputs=enumerate(eRender);inputs=enumerate(eCapture);
    if(renderCombo)fillCombo(renderCombo,outputs,configuration.renderId);
    if(captureCombo)fillCombo(captureCombo,inputs,configuration.captureId);
}
void logStatus(const std::wstring& text) {
    // Keep the most recent 64 status changes; never log microphone samples.
    const auto file=dataDirectory()/L"events.txt";
    std::vector<std::wstring> lines;std::ifstream old(file,std::ios::binary);std::string line;
    while(std::getline(old,line))lines.push_back(wide(line));old.close();
    SYSTEMTIME now{};GetLocalTime(&now);wchar_t stamp[40]{};
    swprintf_s(stamp,L"%04u-%02u-%02u %02u:%02u:%02u ",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
    std::wstring one=text;for(auto& ch:one)if(ch==L'\r'||ch==L'\n')ch=L' ';
    lines.push_back(stamp+one);if(lines.size()>64)lines.erase(lines.begin(),lines.end()-64);
    // UTF-8 output avoids depending on the process locale.
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    for(const auto& item:lines) {
        const auto n=WideCharToMultiByte(CP_UTF8,0,item.data(),static_cast<int>(item.size()),nullptr,0,nullptr,nullptr);
        std::string utf8(n,' ');WideCharToMultiByte(CP_UTF8,0,item.data(),static_cast<int>(item.size()),utf8.data(),n,nullptr,nullptr);out<<utf8<<'\n';
    }
}
std::wstring status() {
    if(!shared.data())return L"아직 오디오에 적용되지 않았습니다.\r\n설치 및 적용을 완료하면 EQ와 반향 제거를 켤 수 있습니다.\r\n설치에는 서명된 오디오 효과 모듈이 필요합니다.";
    LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    auto describe=[&](const wchar_t* title,const Meter& m,bool connected) {
        std::wostringstream s;s<<title<<L": ";
        const auto last=readWide(&m.lastQpc);
        if(!connected)s<<L"장치 연결 대기";
        else if(!last||now.QuadPart<last||now.QuadPart-last>frequency.QuadPart*3)s<<L"오디오 처리 확인 대기";
        else s<<(readWord(&m.enabled)?L"처리 중":L"바이패스")<<L" / "<<readWord(&m.rate)<<L" Hz";
        if(readWord(&m.error))s<<L" / 설정 오류 "<<readWord(&m.error);
        return s.str();
    };
    std::wstring s=describe(L"스피커 EQ",shared.data()->render,containsDevice(outputs,configuration.renderId));
    s+=L"\r\n"+describe(L"마이크 반향 제거",shared.data()->capture,containsDevice(inputs,configuration.captureId));
    const auto& m=shared.data()->capture;
    s+=L"\r\nAEC 참조: "+std::wstring(readWord(&m.reference)?L"처리 활성":L"대기 / 학습 / 참조 없음");
    if(readWord(&m.clipping))s+=L" · 마이크 입력 과부하";
    s+=L"\r\n마이크 처리 지연: 21.33 ms · 공유 모드 전용";
    if(!notice.empty())s+=L"\r\n"+notice;
    return s;
}
void readProfileSummaries() {
    auto describe=[](Channel channel) {
        const auto filename=channel==Channel::left?L"L.txt":L"R.txt";
        auto path=shared.data()?dataDirectory()/filename:std::filesystem::path(L"E:\\Speaker")/filename;
        const auto parsed=parseREWConfigurablePEQFile(path,channel);
        if(!parsed)return std::wstring(L"유효한 EQ 파일이 필요합니다");
        if(parsed.filters.empty())return std::wstring(L"활성 필터 없음");
        std::wostringstream result;
        for(std::size_t i=0;i<parsed.filters.size();++i) {
            if(i)result<<L"   ·   ";
            result<<parsed.filters[i].frequencyHz<<L" Hz / "<<parsed.filters[i].gainDB<<L" dB / Q "<<parsed.filters[i].q;
        }
        return result.str();
    };
    leftSummary=describe(Channel::left);rightSummary=describe(Channel::right);
}
void updateStatus() {
    if(!shared.data()&&shared.open())shared.read(configuration,revision);
    if(shared.data()&&readWord(&shared.data()->sequence)!=revision) {
        if(shared.read(configuration,revision)) {
            readProfileSummaries();
            SendMessageW(eqCheck,BM_SETCHECK,configuration.eqEnabled?BST_CHECKED:BST_UNCHECKED,0);
            SendMessageW(aecCheck,BM_SETCHECK,configuration.aecEnabled?BST_CHECKED:BST_UNCHECKED,0);
            refreshDevices();
        }
    }
    for(auto child:{eqCheck,aecCheck,GetDlgItem(window,leftControl),GetDlgItem(window,rightControl)})EnableWindow(child,shared.data()!=nullptr);
    const auto text=status();
    if(text!=lastStatus) {SetWindowTextW(statusText,text.c_str());InvalidateRect(window,nullptr,FALSE);}
    if(text!=lastStatus) {if(shared.data())logStatus(text);lastStatus=text;}
}
void importFile(Channel channel) {
    if(!shared.data()){error(L"먼저 설치/적용으로 초기 설정을 만드세요.");return;}
    wchar_t path[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=window;
    dialog.lpstrFilter=L"REW EQ (*.txt)\0*.txt\0모든 파일\0*.*\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&dialog))return;
    auto parsed=parseREWConfigurablePEQFile(path,channel);
    if(!parsed){error(wide(parsed.error));return;}
    const auto other=parseREWConfigurablePEQFile(dataDirectory()/(channel==Channel::left?L"R.txt":L"L.txt"),channel==Channel::left?Channel::right:Channel::left);
    if(!other){error(wide(other.error));return;}
    auto next=configuration;std::string message;
    if(!prepareConfiguration(next,channel==Channel::left?parsed.filters:other.filters,channel==Channel::right?parsed.filters:other.filters,message)) {error(wide(message));return;}
    parsed=importREWConfigurablePEQFile(path,dataDirectory()/(channel==Channel::left?L"L.txt":L"R.txt"),channel);
    if(!parsed){error(wide(parsed.error));return;}
    if(!shared.write(next)){error(L"설정 갱신 실패. 다시 시도하세요.");return;}
    readProfileSummaries();notice=channel==Channel::left?L"왼쪽 EQ 가져오기 완료":L"오른쪽 EQ 가져오기 완료";updateStatus();
}
void launchInstaller(bool remove) {
    const auto script=executableDirectory()/(remove?L"uninstall-windows.ps1":L"install-windows.ps1");
    if(!std::filesystem::exists(script)){error(L"실행 파일 옆에 설치 스크립트가 없습니다. CMake install 결과를 사용하세요.");return;}
    std::wstring args=L"-NoProfile -ExecutionPolicy Bypass -File \""+script.wstring()+L"\"";
    if(!remove) {
        const auto r=SendMessageW(renderCombo,CB_GETCURSEL,0,0),c=SendMessageW(captureCombo,CB_GETCURSEL,0,0);
        if(r<0||c<0||static_cast<std::size_t>(r)>=outputs.size()||static_cast<std::size_t>(c)>=inputs.size()){error(L"연결된 출력과 입력 장치를 선택하세요.");return;}
        args+=L" -RenderId \""+outputs[r].id+L"\" -CaptureId \""+inputs[c].id+L"\"";
    }
    SHELLEXECUTEINFOW info{sizeof(info)};info.hwnd=window;info.lpVerb=L"runas";
    info.lpFile=L"powershell.exe";info.lpParameters=args.c_str();info.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&info))error(L"관리자 설치 도구를 시작하지 못했습니다.");
    else {notice=L"설치 도구 실행됨. 실제 처리는 위 상태에서 확인하세요.";updateStatus();}
}
class Notifications final:public IMMNotificationClient {
    std::atomic<ULONG> refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown)&&id!=__uuidof(IMMNotificationClient))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs_;if(!n)delete this;return n;}
    HRESULT changed(){if(window)PostMessageW(window,deviceMessage,0,0);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR,DWORD) override{return changed();}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override{return changed();}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override{return changed();}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR) override{return changed();}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,const PROPERTYKEY) override{return changed();}
};
Notifications* notifications=nullptr;
HWND control(const wchar_t* kind,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
    const auto child=CreateWindowW(kind,text,WS_CHILD|WS_VISIBLE|style|(wcscmp(kind,L"BUTTON")==0?BS_NOTIFY:0),x,y,w,h,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(wcscmp(kind,L"BUTTON")==0&&(style&BS_TYPEMASK)==BS_OWNERDRAW)SetWindowSubclass(child,fluent::hover,2,0);
    SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(fluent::body),TRUE);return child;
}
void layout() {
    RECT client{};GetClientRect(window,&client);pageWidth=std::min(1100,MulDiv(client.right,96,fluent::dpi));
    const int width=pageWidth-64;
    const int visibleHeight=MulDiv(client.bottom,96,fluent::dpi);
    scrollOffset=std::clamp(scrollOffset,0,std::max(0,850-visibleHeight));
    SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS,0,849,static_cast<UINT>(visibleHeight),scrollOffset,0};SetScrollInfo(window,SB_VERT,&scroll,TRUE);
    auto move=[&](int id,int x,int y,int w,int h){MoveWindow(GetDlgItem(window,id),fluent::px(x),fluent::px(y-scrollOffset),fluent::px(w),fluent::px(h),TRUE);};
    move(renderControl,264,142,width-288,240);move(captureControl,264,214,width-288,240);
    move(eqControl,pageWidth-134,315,80,34);move(aecControl,pageWidth-134,521,80,34);
    move(leftControl,pageWidth-202,383,146,34);move(rightControl,pageWidth-202,435,146,34);
    move(statusControl,56,650,width-48,120);move(202,pageWidth-202,604,146,32);
    move(installControl,32,792,152,38);move(removeControl,196,792,152,38);
    InvalidateRect(window,nullptr,FALSE);
}
void paintPage(HDC dc) {
    using namespace fluent;
    RECT client;GetClientRect(window,&client);auto brush=CreateSolidBrush(background);FillRect(dc,&client,brush);DeleteObject(brush);
    const auto saved=SaveDC(dc);SetViewportOrgEx(dc,0,-px(scrollOffset),nullptr);
    auto rect=[](int x,int y,int w,int h){return RECT{px(x),px(y),px(x+w),px(y+h)};};
    auto label=[&](const wchar_t* value,int x,int y,int w,int h,HFONT f,COLORREF c){text(dc,value,rect(x,y,w,h),f,c);};
    const int width=pageWidth-64;
    label(L"사운드",32,22,width,44,title,foreground);
    label(L"내 스피커에 맞는 소리, 더 선명한 목소리.",34,72,width,24,body,secondary);
    label(L"오디오 장치",32,108,width,24,strong,foreground);
    box(dc,rect(32,138,width,62),card,border);box(dc,rect(32,210,width,62),card,border);
    label(L"\xE767",52,152,32,30,icon,accent);label(L"스피커 출력",98,149,150,23,strong,foreground);
    label(L"시스템 전체 EQ",98,173,150,18,small,secondary);
    label(L"\xE720",52,224,32,30,icon,accent);label(L"마이크 입력",98,221,150,23,strong,foreground);
    label(L"입력 1 · 모노",98,245,150,18,small,secondary);
    label(L"소리 조정",32,280,width,24,strong,foreground);
    box(dc,rect(32,310,width,180),card,border);
    label(L"\xE9E9",52,326,32,30,icon,accent);label(L"좌우 독립 이퀄라이저",98,323,400,24,strong,foreground);
    label(shared.data()?L"각 스피커의 REW 필터를 독립적으로 적용합니다.":L"초기 프로필 미리보기 · 설치 후 오디오에 적용됩니다.",98,349,width-110,22,small,secondary);
    label(L"L",56,385,26,28,strong,accent);label(L"왼쪽 스피커",98,381,180,22,strong,foreground);
    label(leftSummary.c_str(),98,404,width-282,20,small,secondary);
    label(L"R",56,439,26,28,strong,accent);label(L"오른쪽 스피커",98,433,180,22,strong,foreground);
    label(rightSummary.c_str(),98,456,width-282,20,small,secondary);
    box(dc,rect(32,502,width,76),card,border);
    label(L"\xE720",52,524,32,30,icon,accent);label(L"마이크 반향 제거",98,514,400,24,strong,foreground);
    label(L"스피커 소리를 줄이고, 내 목소리를 전달합니다.",98,542,width-215,22,small,secondary);
    box(dc,rect(32,594,width,180),card,border);
    label(L"처리 상태",56,607,250,26,strong,foreground);
    label(L"공유 모드 전용 · 닫아도 트레이에서 실행됩니다.",365,794,width-340,34,small,secondary);
    RestoreDC(dc,saved);
}
LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_CREATE:
        window=h;
        fluent::theme(h);fluent::fonts(h);
        renderCombo=control(L"COMBOBOX",L"스피커 출력",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,0,0,0,0,renderControl);
        captureCombo=control(L"COMBOBOX",L"마이크 입력",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,0,0,0,0,captureControl);
        for(auto combo:{renderCombo,captureCombo}) {SendMessageW(combo,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),fluent::px(30));SetWindowSubclass(combo,fluent::combo,3,0);}
        control(L"BUTTON",L"L 파일 가져오기",BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,leftControl);
        control(L"BUTTON",L"R 파일 가져오기",BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,rightControl);
        eqCheck=control(L"BUTTON",L"시스템 EQ 활성화",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,0,0,eqControl);
        aecCheck=control(L"BUTTON",L"마이크 반향 제거 활성화",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,0,0,aecControl);
        SetWindowSubclass(eqCheck,fluent::toggle,1,0);SetWindowSubclass(aecCheck,fluent::toggle,1,0);
        control(L"BUTTON",L"설치 및 적용",BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,installControl);
        control(L"BUTTON",L"원래 설정 복원",BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,removeControl);
        control(L"BUTTON",L"진단 기록 열기",BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,202);
        statusText=control(L"STATIC",L"",0,0,0,0,0,statusControl);
        layout();
        if(shared.open())shared.read(configuration,revision);
        refreshDevices();readProfileSummaries();
        if(!*configuration.renderId)for(std::size_t i=0;i<outputs.size();++i)if(outputs[i].name.find(L"SMSL")!=std::wstring::npos)SendMessageW(renderCombo,CB_SETCURSEL,i,0);
        if(!*configuration.captureId)for(std::size_t i=0;i<inputs.size();++i)if(inputs[i].name.find(L"MOTU")!=std::wstring::npos&&inputs[i].name.find(L"In 1-2")!=std::wstring::npos)SendMessageW(captureCombo,CB_SETCURSEL,i,0);
        SendMessageW(eqCheck,BM_SETCHECK,configuration.eqEnabled?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(aecCheck,BM_SETCHECK,configuration.aecEnabled?BST_CHECKED:BST_UNCHECKED,0);
        tray.cbSize=sizeof(tray);tray.hWnd=h;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
        tray.uCallbackMessage=trayMessage;tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"MacTools — Windows Audio");Shell_NotifyIconW(NIM_ADD,&tray);
        SetTimer(h,1,1000,nullptr);updateStatus();return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;auto dc=BeginPaint(h,&ps);paintPage(dc);EndPaint(h,&ps);return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(w),fluent::foreground);SetBkColor(reinterpret_cast<HDC>(w),fluent::card);return reinterpret_cast<LRESULT>(fluent::cardBrush);
    case WM_DRAWITEM:fluent::button(*reinterpret_cast<DRAWITEMSTRUCT*>(l),w==installControl);return TRUE;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(h,SB_VERT,&si);
        switch(LOWORD(w)) {case SB_LINEUP:scrollOffset-=32;break;case SB_LINEDOWN:scrollOffset+=32;break;case SB_PAGEUP:scrollOffset-=si.nPage;break;case SB_PAGEDOWN:scrollOffset+=si.nPage;break;case SB_THUMBTRACK:scrollOffset=si.nTrackPos;break;case SB_TOP:scrollOffset=0;break;case SB_BOTTOM:scrollOffset=850;break;}
        layout();return 0;
    }
    case WM_MOUSEWHEEL:scrollOffset-=GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*48;layout();return 0;
    case WM_SIZE:if(renderCombo)layout();return 0;
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={fluent::px(780),fluent::px(540)};return 0;
    }
    case WM_SETTINGCHANGE:fluent::theme(h);RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);return 0;
    case WM_DPICHANGED: {
        fluent::fonts(h);for(auto combo:{renderCombo,captureCombo})SendMessageW(combo,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),fluent::px(30));
        for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(fluent::body),TRUE);
        auto r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;
    }
    case WM_COMMAND:
        if(HIWORD(w)==BN_SETFOCUS||HIWORD(w)==CBN_SETFOCUS) {
            RECT focused{},client{};GetWindowRect(reinterpret_cast<HWND>(l),&focused);MapWindowPoints(nullptr,h,reinterpret_cast<POINT*>(&focused),2);GetClientRect(h,&client);
            if(focused.top<0)scrollOffset+=MulDiv(focused.top,96,fluent::dpi)-12;
            else if(focused.bottom>client.bottom)scrollOffset+=MulDiv(focused.bottom-client.bottom,96,fluent::dpi)+12;
            layout();return 0;
        }
        if(HIWORD(w)==CBN_SELCHANGE){InvalidateRect(reinterpret_cast<HWND>(l),nullptr,FALSE);return 0;}
        if(l&&HIWORD(w)!=BN_CLICKED)return 0;
        switch(LOWORD(w)) {
        case leftControl:importFile(Channel::left);break;
        case rightControl:importFile(Channel::right);break;
        case eqControl:case aecControl:
            if(!shared.data()){error(L"먼저 설치/적용을 실행하세요.");SendMessageW(eqCheck,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(aecCheck,BM_SETCHECK,BST_UNCHECKED,0);break;}
            shared.read(configuration,revision);
            configuration.eqEnabled=SendMessageW(eqCheck,BM_GETCHECK,0,0)==BST_CHECKED;
            configuration.aecEnabled=SendMessageW(aecCheck,BM_GETCHECK,0,0)==BST_CHECKED;
            if(!shared.write(configuration))error(L"설정 저장에 실패했습니다.");updateStatus();break;
        case installControl:launchInstaller(false);break;
        case removeControl:launchInstaller(true);break;
        case 201:ShowWindow(h,SW_SHOW);SetForegroundWindow(h);break;
        case 202:ShellExecuteW(h,L"open",(dataDirectory()/L"events.txt").c_str(),nullptr,nullptr,SW_SHOW);break;
        case 203:DestroyWindow(h);break;
        }return 0;
    case deviceMessage:refreshDevices();updateStatus();return 0;
    case WM_TIMER:updateStatus();return 0;
    case trayMessage:
        if(l==WM_LBUTTONDBLCLK){ShowWindow(h,SW_SHOW);SetForegroundWindow(h);}
        if(l==WM_RBUTTONUP) {
            HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,201,L"설정 열기");AppendMenuW(menu,MF_STRING,202,L"상태·복구 기록");AppendMenuW(menu,MF_STRING,203,L"설정 앱 종료 (오디오 효과 유지)");
            POINT p;GetCursorPos(&p);SetForegroundWindow(h);TrackPopupMenu(menu,TPM_RIGHTBUTTON,p.x,p.y,0,h,nullptr);DestroyMenu(menu);
        }return 0;
    case WM_CLOSE:ShowWindow(h,SW_HIDE);return 0;
    case WM_DESTROY:Shell_NotifyIconW(NIM_DELETE,&tray);KillTimer(h,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,message,w,l);
}
int initialize(int argc,LPWSTR* argv) {
    if(argc!=4)return 2;
    refreshDevices();if(!containsDevice(outputs,argv[2])||!containsDevice(inputs,argv[3]))return 3;
    if(!shared.open(true)||!shared.read(configuration,revision))return 4;
    const auto directory=dataDirectory();
    auto left=parseREWConfigurablePEQFile(directory/L"L.txt",Channel::left);
    auto right=parseREWConfigurablePEQFile(directory/L"R.txt",Channel::right);
    if(!left)left=importREWConfigurablePEQFile(L"E:\\Speaker\\L.txt",directory/L"L.txt",Channel::left);
    if(!right)right=importREWConfigurablePEQFile(L"E:\\Speaker\\R.txt",directory/L"R.txt",Channel::right);
    if(!left||!right)return 5;
    std::string error;
    if(!prepareConfiguration(configuration,left.filters,right.filters,error))return 6;
    if(wcslen(argv[2])>=512||wcslen(argv[3])>=512)return 7;
    wcscpy_s(configuration.renderId,argv[2]);wcscpy_s(configuration.captureId,argv[3]);
    configuration.eqEnabled=configuration.aecEnabled=1;
    return shared.write(configuration)?0:8;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput gdiplusInput;ULONG_PTR gdiplusToken=0;Gdiplus::GdiplusStartup(&gdiplusToken,&gdiplusInput,nullptr);
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 1;
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator)))){CoUninitialize();return 1;}
    int argc=0;auto** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    int result=0;
    if(argc>1&&wcscmp(argv[1],L"--initialize")==0)result=initialize(argc,argv);
    else {
        HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\MacToolsWindowsUI");
        if(GetLastError()==ERROR_ALREADY_EXISTS){if(mutex)CloseHandle(mutex);result=0;}
        else {
            WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=instance;wc.lpszClassName=L"MacToolsWindows";
            wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);
            RECT workArea{};SystemParametersInfoW(SPI_GETWORKAREA,0,&workArea,0);
            const int initialHeight=std::min(MulDiv(900,GetDpiForSystem(),96),static_cast<int>(workArea.bottom-workArea.top)-40);
            HWND h=CreateWindowW(wc.lpszClassName,L"MacTools — Windows Audio",WS_OVERLAPPEDWINDOW|WS_VSCROLL|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(900,GetDpiForSystem(),96),initialHeight,nullptr,nullptr,instance,nullptr);
            if(h) {
                notifications=new Notifications;enumerator->RegisterEndpointNotificationCallback(notifications);
                ShowWindow(h,show);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
                enumerator->UnregisterEndpointNotificationCallback(notifications);notifications->Release();window=nullptr;
            } else result=1;
            if(mutex)CloseHandle(mutex);
        }
    }
    LocalFree(argv);enumerator->Release();CoUninitialize();fluent::release();Gdiplus::GdiplusShutdown(gdiplusToken);return result;
}
