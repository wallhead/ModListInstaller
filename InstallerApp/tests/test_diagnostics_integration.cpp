#include "diagnostics/Diagnostics.h"
#include "manifest/Json.h"

#include <windows.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace modlist;
using namespace modlist::diagnostics;
void Require(bool value,const std::string& why) {if(!value)throw std::runtime_error(why);}

int wmain(int argc,wchar_t** argv) {
  try {
    if(argc!=3)throw std::runtime_error("Usage: diagnostics_integration_tests <source-parent> <destination-parent>");
    Config config;config.sourceParent=argv[1];config.destinationParent=argv[2];config.smoke=true;config.sizeGiB=10;
    const auto unique=L"integration-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    config.sourceParent/=unique;config.destinationParent/=unique;
    Require(std::filesystem::create_directory(config.sourceParent),"Test source parent already exists");
    Require(std::filesystem::create_directory(config.destinationParent),"Test destination parent already exists");
    const auto testSource=config.sourceParent;
    const auto testDestination=config.destinationParent;
    config.sourceParent/=L"new source\\nested";
    config.destinationParent/=L"new target\\nested";
    const auto destinationParent=config.destinationParent;
    std::atomic_bool cancel{true};
    const auto cancelled=RunBenchmark(config,cancel);
    Require(!cancelled.ok && cancelled.cancelled && cancelled.sourceRun.empty(),"Pre-cancelled benchmark created data or reported success");
    Require(!std::filesystem::exists(config.sourceParent) && !std::filesystem::exists(config.destinationParent),"Pre-cancelled benchmark created missing parent folders");
    cancel=false;config.reversePass=true;
    std::promise<void> finalProgressEntered, releaseFinalProgress;
    auto releaseFinal=releaseFinalProgress.get_future().share();
    std::map<std::string, Progress> extractionFinals;
    auto benchmark=std::async(std::launch::async,[&] {
      return RunBenchmark(config,cancel,[&](const Progress& progress) {
        if(!progress.stage.starts_with("extract_") || progress.message!="Finished")return;
        extractionFinals[progress.stage]=progress;
        if(progress.stage=="extract_pipeline-1") {
          finalProgressEntered.set_value();
          releaseFinal.wait();
        }
      });
    });
    const bool reachedFinal=finalProgressEntered.get_future().wait_for(std::chrono::seconds(60))==std::future_status::ready;
    const bool drainBlocked=reachedFinal && benchmark.wait_for(std::chrono::milliseconds(150))==std::future_status::timeout;
    releaseFinalProgress.set_value();
    const auto outcome=benchmark.get();
    Require(outcome.ok,!outcome.error.empty()?outcome.error:"Benchmark did not succeed");
    Require(drainBlocked,"Test did not block final progress delivery");
    Require(std::filesystem::is_directory(config.sourceParent) && std::filesystem::is_directory(config.destinationParent),"Missing parent folders were not created");
    std::ifstream file(outcome.logFolder/"results.json",std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(file)),{});
    const auto parsed=ParseJson(json);Require(parsed.ok(),"Report is not valid JSON");
    const auto* stages=parsed.value().Find("stages");Require(stages && stages->IsArray(),"Missing stage reports");
    int extracts=0,verifications=0,archiveHashes=0;
    for(const auto& stage:stages->AsArray()) {
      Require(stage.Find("status")->AsString()=="ok","A smoke stage failed");
      const auto name=stage.Find("name")->AsString();
      Require(name!="source_sha256_read","Benchmark still rereads and hashes every generated source file");
      if(name.starts_with("extract_")) {
        ++extracts;Require(stage.Find("logical_bytes")->AsNumber()==64*1024*1024,"Extraction byte total is false");
        const auto final=extractionFinals.find(name);
        Require(final!=extractionFinals.end(),"Final extraction progress was lost");
        Require(final->second.bytes==64*1024*1024 && final->second.files==515 && final->second.percent==100,"Final extraction progress counters are false");
        Require(std::abs(stage.Find("seconds")->AsNumber()-final->second.elapsedSeconds)<1e-9,"Stage timing includes final progress/log draining after its final measurement");
      }
      if(name=="archive_create") {
        Require(stage.Find("logical_bytes")->AsNumber()==64*1024*1024,"Archive creation must report logical input bytes");
        Require(stage.Find("sample_progress_basis")->AsString()=="child_process_read_proxy","Compression progress incorrectly follows compressed output instead of source reads");
      }
      if(name.starts_with("extract_7zip"))Require(stage.Find("sample_progress_basis")->AsString()=="child_process_write_proxy","Native extraction progress no longer follows unpacked output");
      if(name=="archive_sha256") {++archiveHashes;Require(stage.Find("logical_bytes")->AsNumber()>0,"Archive hashing was removed");}
      if(name.starts_with("verify_")) {
        ++verifications;
        Require(stage.Find("logical_bytes")->AsNumber()==0,"Metadata checks falsely report payload throughput");
        Require(stage.Find("process_read_bytes")->AsNumber()==0,"Metadata checks still reread file contents");
        Require(stage.Find("files")->AsNumber()==515,"Metadata checks missed files");
      }
    }
    Require(extracts==4 && verifications==5 && archiveHashes==1,"Extraction, metadata checks or archive hashing stages were skipped");
    Require(!parsed.value().Find("per_file_sha256")->AsBool(),"Report falsely claims per-file hashing is enabled");
    for(const auto& record:parsed.value().Find("pipeline_diagnostics")->AsArray())
      Require(record.Find("counters")->Find("hashNanoseconds")->AsNumber()>0,"Real pipeline archive validation is disabled");
    std::ifstream progressFile(outcome.logFolder/"diagnostics.log",std::ios::binary);
    const std::string progressLog((std::istreambuf_iterator<char>(progressFile)),{});
    const auto validationBytes=static_cast<uint64_t>(parsed.value().Find("archive_bytes")->AsNumber());
    Require(progressLog.find("; validation_bytes="+std::to_string(validationBytes)+"; extraction_percent=100")!=std::string::npos,"Final pipeline validation progress was lost from the log");
    std::cout << "PASS: complete smoke benchmark, both extraction orders, metadata checks only\n";
    std::cout << "PASS: missing nested parent folders are created automatically\n";
    std::cout << "Report: " << outcome.logFolder << '\n';

    for(const bool changeSize:{false,true}) {
      const auto corruptParent=destinationParent/(changeSize?L"size-case":L"same-size-case");
      Require(std::filesystem::create_directory(corruptParent),"Mutation test parent already exists");
      config.destinationParent=corruptParent;config.reversePass=false;bool changed=false;
      const auto altered=RunBenchmark(config,cancel,[&](const Progress& progress){
        if(progress.stage!="verify_copy" || progress.message!="Started")return;
        for(const auto& entry:std::filesystem::directory_iterator(corruptParent)) {
          if(!entry.is_directory())continue;
          const auto path=entry.path()/"copy"/"bulk"/"large-0.bin";
          if(!std::filesystem::is_regular_file(path))continue;
          if(changeSize) {std::filesystem::resize_file(path,7);changed=true;}
          else {
            std::fstream data(path,std::ios::binary|std::ios::in|std::ios::out);
            data.seekg(17);char byte{};data.read(&byte,1);byte^=1;data.seekp(17);data.write(&byte,1);data.flush();changed=data.good();
          }
        }
      });
      Require(changed,"Test did not alter its copied file");
      if(changeSize)Require(!altered.ok && altered.error.find("File size mismatch")!=std::string::npos,"Wrong-sized copy was not rejected");
      else Require(altered.ok,"Benchmark still checks copied file contents instead of metadata only");
    }
    std::cout << "PASS: no copied-content checks; metadata still rejects wrong file sizes\n";

    config.destinationParent=destinationParent;config.sevenZipFirst=true;
    const auto stopped=RunBenchmark(config,cancel,[&](const Progress& progress){
      if(progress.stage=="extract_7zip-1" && progress.message=="Started")cancel=true;
    });
    Require(stopped.cancelled && !stopped.ok,"Cancelling native extraction did not stop the benchmark");
    std::cout << "PASS: native child cancellation returns a partial report\n";
    cancel=false;

    config.sevenZipFirst=false;
    const auto pipelineStopped=RunBenchmark(config,cancel,[&](const Progress& progress){
      if(progress.stage=="extract_pipeline-1" && progress.message=="Started")cancel=true;
    });
    Require(pipelineStopped.cancelled && !pipelineStopped.ok,"Cancelling pipeline extraction did not stop the benchmark");
    std::ifstream stoppedFile(pipelineStopped.logFolder/"results.json",std::ios::binary);
    const auto stoppedJson=ParseJson(std::string((std::istreambuf_iterator<char>(stoppedFile)),{}));
    Require(stoppedJson.ok(),"Cancelled pipeline report is not valid JSON");
    const auto& stoppedStages=stoppedJson.value().Find("stages")->AsArray();
    Require(!stoppedStages.empty(),"Cancelled pipeline lost its stage report");
    const auto& stoppedStage=stoppedStages.back();
    Require(stoppedStage.Find("name")->AsString()=="extract_pipeline-1" && stoppedStage.Find("status")->AsString()=="failed","Cancelled pipeline stage incorrectly reported success");
    Require(stoppedStage.Find("logical_bytes")->AsNumber()==0 && stoppedStage.Find("files")->AsNumber()==0,"Cancelled pipeline fabricated completed output counters");
    std::cout << "PASS: pipeline cancellation retains a failed stage and partial counters\n";
    cancel=false;

    bool reportBlocked=false;
    const auto blocked=RunBenchmark(config,cancel,[&](const Progress& progress){
      if(progress.stage!="verify_pipeline-1" || progress.message!="Finished")return;
      reportBlocked=std::filesystem::create_directory(progress.logFolder/"results.txt");
    });
    Require(reportBlocked && !blocked.ok,"Unwritable text report falsely reported success");
    std::ifstream blockedFile(blocked.logFolder/"results.json",std::ios::binary);
    const auto blockedJson=ParseJson(std::string((std::istreambuf_iterator<char>(blockedFile)),{}));
    Require(blockedJson.ok() && !blockedJson.value().Find("ok")->AsBool(),"JSON report falsely claims success after text report failure");
    std::cout << "PASS: report write failure remains truthful in JSON\n";

    for(const bool sourceBlocked:{true,false}) {
      const auto path=(sourceBlocked?testSource:testDestination)/"existing-file.txt";
      {std::ofstream sentinel(path,std::ios::binary);sentinel << "Do not overwrite";Require(sentinel.good(),"Cannot prepare collision fixture");}
      Config invalid=config;
      if(sourceBlocked)invalid.sourceParent=path;else invalid.destinationParent=path;
      const auto rejected=RunBenchmark(invalid,cancel);
      const std::string role=sourceBlocked?"source":"destination";
      Require(!rejected.ok && !rejected.cancelled && rejected.sourceRun.empty(),"Existing file was accepted as a parent folder");
      Require(rejected.error.find(role)!=std::string::npos && rejected.error.find("existing-file.txt")!=std::string::npos && rejected.error.find("Windows error")!=std::string::npos,"Folder error does not identify its role, path and Windows code");
      std::ifstream sentinel(path,std::ios::binary);
      Require(std::string((std::istreambuf_iterator<char>(sentinel)),{})=="Do not overwrite","Existing file was modified");
    }
    std::cout << "PASS: invalid source/destination folders report exact paths without modifying files\n";
    return 0;
  }catch(const std::exception& ex){std::cerr << "FAIL: " << ex.what() << '\n';return 1;}
}
