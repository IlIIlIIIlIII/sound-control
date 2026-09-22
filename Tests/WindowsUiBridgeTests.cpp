// Exercises the real UI bridge against isolated configuration storage.
#include "../Sources/Windows/UiBridge.cpp"
#include <iostream>
namespace {
int failures=0;
void expect(bool value,const char* message) { if(!value){std::cerr<<message<<'\n';++failures;} }
}
int main() {
    {
        Meter m{}; LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        m.lastQpc=now.QuadPart; m.enabled=1; m.reference=0;
        expect(meter(m,true,true).find(L"반향 제거 확인 안 됨")!=std::wstring::npos,"Live capture without reference must not claim echo removal");
        m.reference=1;
        expect(meter(m,true,true).find(L"처리 중")!=std::wstring::npos,"Live capture with reference reports processing");
        m.enabled=0;
        expect(meter(m,true,true).find(L"\"enabled\":false")!=std::wstring::npos,"OS-bypassed capture must not report enabled");
        m.enabled=1;
        m.lastQpc=0;
        expect(meter(m,true,true).find(L"오디오 처리 확인 대기")!=std::wstring::npos,"Stale capture must not claim processing even with old reference");
        expect(meter(m,true,true).find(L"\"enabled\":false")!=std::wstring::npos,"Stale enabled flag must not report a live effect");
    }
    const auto directory=dataDirectory();
    std::filesystem::create_directories(directory);
    {
        void* session=MT_Create();
        expect(session!=nullptr,"Create bridge session");
        if(!session)return 1;
        std::wstring snapshot=MT_Snapshot(session);
        expect(snapshot.find(L"\"ready\":false")!=std::wstring::npos,"Uninitialized state is not ready");
        expect(wcslen(MT_SetEnabled(session,1,1))>0,"Do not toggle before initialization");
        expect(!std::filesystem::exists(directory/L"state-v1.bin"),"Snapshot/toggle do not create settings");
        MT_Destroy(session);
    }
    {
        std::ofstream(directory/L"L.txt")<<"Configurable_PEQ\n1 True Auto PK 66 -12 7.4\n";
        std::ofstream(directory/L"R.txt")<<"Configurable_PEQ\n1 True Auto PK 65 -8.4 8\n";
        SharedFile file;Configuration config;LONG revision=0;
        expect(file.open(true),"Create isolated configuration");
        auto left=parseREWConfigurablePEQFile(directory/L"L.txt",Channel::left);
        auto right=parseREWConfigurablePEQFile(directory/L"R.txt",Channel::right);
        std::string error;
        expect(prepareConfiguration(config,left.filters,right.filters,error),"Prepare actual DSP coefficients");
        expect(file.write(config),"Write valid configuration");
        void* session=MT_Create();
        std::wstring snapshot=MT_Snapshot(session);
        expect(snapshot.find(L"\"ready\":true")!=std::wstring::npos,"Read initialized state");
        expect(snapshot.find(L"\"frequency\":66")!=std::wstring::npos,"Expose real left profile");
        expect(wcslen(MT_SetEnabled(session,1,0))==0,"Enable EQ independently");
        expect(file.read(config,revision)&&config.eqEnabled==1&&config.aecEnabled==0,"Persist toggle to shared ABI");
        const auto staleRevision=revision;
        expect(file.write(config),"Advance settings revision");
        auto stale=config;stale.aecEnabled=1;
        expect(!file.write(stale,staleRevision),"Recovery cannot overwrite a concurrent toggle");
        expect(file.read(config,revision)&&config.aecEnabled==0,"Stale recovery preserves toggle");
        expect(MT_RebindRender(L"old",L"not-an-endpoint",L"capture")==3,"Recovery rejects disconnected endpoint");
        const auto activeOutputs=devices(eRender);
        if(!activeOutputs.empty()) {
            expect(MT_RebindRender(L"old",activeOutputs[0].id.c_str(),L"")==9,"Recovery rejects stale original endpoint");
            expect(MT_RebindRender(L"",activeOutputs[0].id.c_str(),L"wrong")==9,"Recovery rejects changed capture selection");
            expect(MT_RebindRender(L"",activeOutputs[0].id.c_str(),L"")==0,"Recovery binds active output in isolated config");
            expect(file.read(config,revision)&&config.eqEnabled==1&&config.aecEnabled==0&&config.count[0]==1,"Recovery preserves toggles and EQ");
            expect(MT_RebindRender(L"",activeOutputs[0].id.c_str(),L"")==0,"Recovery can retry after interrupted commit");
        }
        std::ofstream(directory/L"invalid.txt")<<"invalid profile";
        expect(wcslen(MT_Import(session,0,(directory/L"invalid.txt").c_str()))>0,"Reject invalid filter");
        left=parseREWConfigurablePEQFile(directory/L"L.txt",Channel::left);
        expect(left&&left.filters.size()==1&&left.filters[0].frequencyHz==66,"Failed import preserves prior filter");
        std::ofstream(directory/L"valid.txt")<<"Configurable_PEQ\n1 True Auto PK 120 -3 2\n";
        expect(wcslen(MT_Import(session,0,(directory/L"valid.txt").c_str()))==0,"Import valid REW filter");
        snapshot=MT_Snapshot(session);
        expect(snapshot.find(L"\"frequency\":120")!=std::wstring::npos,"Imported profile appears in snapshot");
        expect(wcslen(MT_Import(session,8,L"unused"))>0,"Reject invalid channel");
        expect(MT_Initialize(L"not-an-endpoint",L"not-an-endpoint")==3,"Reject nonexistent device IDs");
        expect(file.read(config,revision)&&config.eqEnabled==1&&config.aecEnabled==0,"Failed initialize preserves config");
        MT_Destroy(session);
    }
    std::filesystem::remove_all(directory);
    if(!failures)std::cout<<"WinUI bridge tests passed (isolated settings; no endpoint changes).\n";
    return failures?1:0;
}
