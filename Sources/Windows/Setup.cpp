#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <filesystem>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    const bool rebind=args&&count==5&&wcscmp(args[1],L"--rebind-render")==0;
    if(!rebind&&(!args||count!=4||wcscmp(args[1],L"--initialize")!=0)){LocalFree(args);return 2;}
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))){LocalFree(args);return 1;}
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    const auto library=std::filesystem::path(path).parent_path()/L"ui"/L"PersonalToolsBridge.dll";
    auto module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Initialize=int(__cdecl*)(const wchar_t*,const wchar_t*);
    auto initialize=module?reinterpret_cast<Initialize>(GetProcAddress(module,"MT_Initialize")):nullptr;
    using Rebind=int(__cdecl*)(const wchar_t*,const wchar_t*,const wchar_t*);
    auto repair=module?reinterpret_cast<Rebind>(GetProcAddress(module,"MT_RebindRender")):nullptr;
    const int result=rebind?(repair?repair(args[2],args[3],args[4]):1):(initialize?initialize(args[2],args[3]):1);
    if(module)FreeLibrary(module);CoUninitialize();LocalFree(args);return result;
}
