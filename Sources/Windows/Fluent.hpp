#pragma once
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>

// Fluent visual treatment over native controls: keyboard navigation and the
// native checkbox/combo accessibility providers remain intact.
namespace fluent {
inline UINT dpi=96;
inline HFONT body=nullptr,small=nullptr,strong=nullptr,title=nullptr,icon=nullptr;
inline HBRUSH cardBrush=nullptr;
inline COLORREF background,card,foreground,secondary,border,accent;
inline bool dark=false;
inline int px(int n){return MulDiv(n,static_cast<int>(dpi),96);}
inline HFONT font(int size,int weight=FW_NORMAL,const wchar_t* family=L"Segoe UI Variable Text") {
    return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);
}
inline void theme(HWND window) {
    DWORD light=1,size=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
    dark=!light;
    background=dark?RGB(32,32,32):RGB(243,243,243);card=dark?RGB(43,43,43):RGB(255,255,255);
    foreground=dark?RGB(246,246,246):RGB(28,28,28);secondary=dark?RGB(191,191,191):RGB(96,96,96);
    border=dark?RGB(65,65,65):RGB(226,226,226);accent=dark?RGB(117,190,255):RGB(0,95,184);
    HIGHCONTRASTW hc{sizeof(hc)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
    if(hc.dwFlags&HCF_HIGHCONTRASTON){background=card=GetSysColor(COLOR_WINDOW);foreground=secondary=GetSysColor(COLOR_WINDOWTEXT);accent=GetSysColor(COLOR_HIGHLIGHT);border=foreground;}
    if(cardBrush)DeleteObject(cardBrush);cardBrush=CreateSolidBrush(card);
    BOOL useDark=dark;DwmSetWindowAttribute(window,20,&useDark,sizeof(useDark));DWORD corners=2;DwmSetWindowAttribute(window,33,&corners,sizeof(corners));
}
inline void fonts(HWND window) {
    dpi=GetDpiForWindow(window);
    for(auto f:{body,small,strong,title,icon})if(f)DeleteObject(f);
    body=font(14);small=font(12);strong=font(14,FW_SEMIBOLD);title=font(30,FW_SEMIBOLD);icon=font(23,FW_NORMAL,L"Segoe Fluent Icons");
}
inline Gdiplus::Color color(COLORREF c){return Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c));}
inline void box(HDC dc,RECT r,COLORREF fill,COLORREF line,int radius=8) {
    Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x=static_cast<float>(r.left)+.5f,y=static_cast<float>(r.top)+.5f,w=static_cast<float>(r.right-r.left)-1,h=static_cast<float>(r.bottom-r.top)-1,d=static_cast<float>(px(radius)*2);
    Gdiplus::GraphicsPath p;p.AddArc(x,y,d,d,180,90);p.AddArc(x+w-d,y,d,d,270,90);p.AddArc(x+w-d,y+h-d,d,d,0,90);p.AddArc(x,y+h-d,d,d,90,90);p.CloseFigure();
    Gdiplus::SolidBrush b(color(fill));Gdiplus::Pen pen(color(line));g.FillPath(&b,&p);g.DrawPath(&pen,&p);
}
inline void text(HDC dc,const wchar_t* value,RECT r,HFONT f,COLORREF c,UINT flags=DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS) {
    auto old=SelectObject(dc,f);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,c);DrawTextW(dc,value,-1,&r,flags);SelectObject(dc,old);
}
inline LRESULT CALLBACK toggle(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);FillRect(dc,&r,cardBrush);
        const bool on=SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED;
        RECT track{px(4),px(7),px(44),px(27)};box(dc,track,on?accent:card,on?accent:secondary,10);
        Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Gdiplus::SolidBrush b(color(on?(dark?RGB(25,25,25):RGB(255,255,255)):secondary));
        g.FillEllipse(&b,px(on?27:8),px(11),px(12),px(12));
        RECT label{px(56),0,r.right,r.bottom};text(dc,on?L"켬":L"끔",label,body,foreground);
        if(GetFocus()==h){InflateRect(&r,-px(1),-px(1));DrawFocusRect(dc,&r);}EndPaint(h,&ps);return 0;
    }
    const auto result=DefSubclassProc(h,msg,w,l);
    if(msg==BM_SETCHECK||msg==WM_SETFOCUS||msg==WM_KILLFOCUS||msg==WM_ENABLE)InvalidateRect(h,nullptr,FALSE);
    return result;
}
inline LRESULT CALLBACK hover(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(msg==WM_MOUSEMOVE&&!GetPropW(h,L"FluentHover")) {SetPropW(h,L"FluentHover",reinterpret_cast<HANDLE>(1));TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);InvalidateRect(h,nullptr,FALSE);}
    if(msg==WM_MOUSELEAVE||msg==WM_NCDESTROY){RemovePropW(h,L"FluentHover");InvalidateRect(h,nullptr,FALSE);}
    return DefSubclassProc(h,msg,w,l);
}
inline LRESULT CALLBACK combo(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps;auto dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);FillRect(dc,&r,cardBrush);box(dc,r,card,border,4);
        wchar_t label[512]{};GetWindowTextW(h,label,512);auto t=r;t.left+=px(12);t.right-=px(34);text(dc,label,t,body,foreground);
        t=r;t.left=r.right-px(28);text(dc,L"\xE70D",t,icon,secondary);
        if(GetFocus()==h){auto focus=r;InflateRect(&focus,-px(3),-px(3));DrawFocusRect(dc,&focus);}
        EndPaint(h,&ps);return 0;
    }
    const auto result=DefSubclassProc(h,msg,w,l);
    if(msg==WM_SETFOCUS||msg==WM_KILLFOCUS||msg==CB_SETCURSEL||msg==WM_KEYDOWN||msg==WM_LBUTTONUP)InvalidateRect(h,nullptr,FALSE);
    return result;
}
inline void button(const DRAWITEMSTRUCT& item,bool primary) {
    const bool pressed=(item.itemState&ODS_SELECTED)!=0;
    const bool enabled=IsWindowEnabled(item.hwndItem)!=FALSE;
    const auto fill=primary&&enabled?accent:((pressed||GetPropW(item.hwndItem,L"FluentHover"))?background:card);
    box(item.hDC,item.rcItem,fill,primary&&enabled?accent:border,4);
    wchar_t label[256]{};GetWindowTextW(item.hwndItem,label,256);
    text(item.hDC,label,item.rcItem,body,!enabled?secondary:(primary?(dark?RGB(15,15,15):RGB(255,255,255)):foreground),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(item.itemState&ODS_FOCUS){auto r=item.rcItem;InflateRect(&r,-px(3),-px(3));DrawFocusRect(item.hDC,&r);}
}
inline void release(){for(auto f:{body,small,strong,title,icon})if(f)DeleteObject(f);if(cardBrush)DeleteObject(cardBrush);}
}
