#define NOMINMAX
#include "diagnostics/Diagnostics.h"
#include "diagnostics/PipelineProgressSnapshot.h"
#include "common/PathText.h"
#include "extractor/PipelinedSevenZipExtractor.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Json.h"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace modlist::diagnostics {
namespace {
using Clock = std::chrono::steady_clock;
using J = JsonValue;
constexpr uint64_t GiB = 1024ull * 1024 * 1024;
constexpr double MiB = 1024.0 * 1024;

struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

std::string ErrorText(const std::string& operation, const std::filesystem::path& path, DWORD code = GetLastError()) {
  return operation + ": " + PathToUtf8(path) + " (Windows error " + std::to_string(code) + ")";
}

std::string Stamp() {
  SYSTEMTIME t{}; GetLocalTime(&t);
  char buffer[40]{};
  sprintf_s(buffer, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
  return buffer;
}

std::filesystem::path ModuleFolder() {
  std::vector<wchar_t> buffer(32768);
  const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (!n || n == buffer.size()) throw std::runtime_error("Unable to locate diagnostics executable");
  return std::filesystem::path(std::wstring(buffer.data(), n)).parent_path();
}

std::string Escape(const std::string& value) {
  std::ostringstream out; out << '"';
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
    else out << c;
  }
  return out.str() + '"';
}

std::string Json(const J& value) {
  if (value.IsNull()) return "null";
  if (value.IsBool()) return value.AsBool() ? "true" : "false";
  if (value.IsNumber()) {std::ostringstream out;out.imbue(std::locale::classic());out << std::setprecision(16) << value.AsNumber();return out.str();}
  if (value.IsString()) return Escape(value.AsString());
  std::string out; bool first = true;
  if (value.IsArray()) {
    out = "[";
    for (const auto& item : value.AsArray()) {if (!first) out += ',';first = false;out += Json(item);}
    return out + ']';
  }
  out = "{";
  for (const auto& [key,item] : value.AsObject()) {if (!first) out += ',';first = false;out += Escape(key) + ':' + Json(item);}
  return out + '}';
}

J Number(uint64_t n) { return J(static_cast<double>(n)); }
J PathValue(const std::filesystem::path& p) { return J(PathToUtf8(p)); }

struct Log {
  std::mutex mutex;
  std::filesystem::path folder;
  std::ofstream text, samples;
  explicit Log(const std::filesystem::path& folder) : folder(folder) {
    text.open(folder / "diagnostics.log", std::ios::binary);
    samples.open(folder / "samples.csv", std::ios::binary);
    text.imbue(std::locale::classic()); samples.imbue(std::locale::classic());
    if (!text || !samples) throw std::runtime_error("Unable to open diagnostic logs");
    samples << "time,stage,elapsed_seconds,logical_bytes,files,logical_mib_s,cpu_percent_all_cores,working_set_mib,private_mib,process_read_mib_s,process_write_mib_s\r\n";
  }
  void Line(const std::string& line) {
    std::lock_guard lock(mutex); text << Stamp() << " " << line << "\r\n"; text.flush();
  }
  bool Healthy() {
    std::lock_guard lock(mutex); return text.good() && samples.good();
  }
};

struct Algorithm {
  BCRYPT_ALG_HANDLE handle{};
  Algorithm() {if (BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)) throw std::runtime_error("SHA-256 provider failed");}
  ~Algorithm() {if(handle) BCryptCloseAlgorithmProvider(handle,0);}
};
struct Hash {
  BCRYPT_HASH_HANDLE handle{};
  explicit Hash(BCRYPT_ALG_HANDLE algorithm) {if(BCryptCreateHash(algorithm,&handle,nullptr,0,nullptr,0,0)) throw std::runtime_error("SHA-256 initialization failed");}
  ~Hash() {if(handle) BCryptDestroyHash(handle);}
  void Add(const void* bytes, size_t size) {if(BCryptHashData(handle,reinterpret_cast<PUCHAR>(const_cast<void*>(bytes)),static_cast<ULONG>(size),0)) throw std::runtime_error("SHA-256 update failed");}
  std::string Finish() {
    uint8_t digest[32]{};
    if(BCryptFinishHash(handle,digest,sizeof(digest),0)) throw std::runtime_error("SHA-256 finish failed");
    std::ostringstream out;out << std::hex << std::setfill('0');for(auto byte:digest)out << std::setw(2) << static_cast<unsigned>(byte);return out.str();
  }
};

uint64_t CpuTicks(HANDLE process) {
  FILETIME a{},b{},kernel{},user{};
  if (!GetProcessTimes(process,&a,&b,&kernel,&user)) return 0;
  ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;
}
IO_COUNTERS Io(HANDLE process) {IO_COUNTERS io{};GetProcessIoCounters(process,&io);return io;}

struct Run {
  Config config;
  std::atomic_bool& cancel;
  ProgressCallback callback;
  Log& log;
  J::Array stages;
  void Check() {if(cancel.load()) throw std::runtime_error("Cancelled by user");if(!log.Healthy()) throw std::runtime_error("Diagnostic log could not be written");}
  void Emit(Progress progress) {progress.logFolder=log.folder;if(callback) {try {callback(progress);} catch(...) {}}}
};

class Stage {
public:
  std::atomic<uint64_t> bytes{0}, files{0};
  Stage(Run& run, std::string name, uint64_t total, HANDLE process = GetCurrentProcess(),
        ProgressBasis basis = ProgressBasis::LogicalBytes, PipelineExtractionDiagnostics* pipeline = nullptr,
        PipelineProgressSnapshot* pipelineProgress = nullptr)
      : run_(run), name_(std::move(name)), total_(total), process_(process), basis_(basis), pipeline_(pipeline), pipelineProgress_(pipelineProgress),
        start_(Clock::now()), last_(start_), cpuStart_(CpuTicks(process)), cpuLast_(cpuStart_), ioStart_(Io(process)), ioLast_(ioStart_) {
    run_.log.Line("START " + name_ + "; planned_bytes=" + std::to_string(total_));
    Progress progress{name_,"Started",0,total_};progress.basis=basis_;run_.Emit(std::move(progress));
    sampler_ = std::thread([this] {
      std::unique_lock lock(waitMutex_);
      while(!stop_.wait_for(lock,std::chrono::seconds(1),[&]{return finished_;})) {lock.unlock();Sample(false);lock.lock();}
    });
  }
  ~Stage() {if(!recorded_) Finish("failed");}
  void Finish(const std::string& status = "ok",std::optional<uint64_t> completedBytes={},std::optional<uint64_t> completedFiles={}) {
    if(recorded_)return;
    // Sink draining (including an in-flight sampler) is not extraction work.
    const auto finishedAt=Clock::now();
    const double seconds=std::chrono::duration<double>(finishedAt-start_).count();
    const auto io=Io(process_);const auto cpu=CpuTicks(process_);
    {std::lock_guard lock(waitMutex_);finished_=true;}stop_.notify_all();if(sampler_.joinable())sampler_.join();
    if(completedBytes)bytes=*completedBytes;if(completedFiles)files=*completedFiles;
    Sample(true,finishedAt);recorded_=true;
    const uint64_t count=bytes.load();const uint64_t fileCount=files.load();
    const double average=seconds>0?static_cast<double>(count)/MiB/seconds:0;
    run_.stages.emplace_back(J::Object{{"name",J(name_)},{"status",J(status)},{"seconds",J(seconds)},
      {"logical_bytes",Number(count)},{"files",Number(fileCount)},{"average_mib_s",J(average)},
      {"peak_sampled_mib_s",hasSpeedSample_?J(peak_):J()},{"files_per_second",J(seconds>0?fileCount/seconds:0)},
      {"cpu_seconds",J(CounterDelta(cpu,cpuStart_)/10000000.0)},
      {"peak_working_set_mib",J(peakWorking_)},{"peak_private_mib",J(peakPrivate_)},
      {"process_read_bytes",Number(CounterDelta(io.ReadTransferCount,ioStart_.ReadTransferCount))},
      {"process_write_bytes",Number(CounterDelta(io.WriteTransferCount,ioStart_.WriteTransferCount))},
      {"sample_progress_basis",J(std::string(ProgressBasisName(basis_)))}});
    std::ostringstream summary;summary << "END " << name_ << "; status=" << status << "; seconds=" << seconds << "; bytes=" << count << "; files=" << fileCount << "; average_MiB_s=" << average << "; peak_sampled_MiB_s=" << peak_;
    run_.log.Line(summary.str());
  }
private:
  void Sample(bool final,Clock::time_point now=Clock::now()) {
    const double elapsed=std::chrono::duration<double>(now-start_).count();
    const double interval=std::chrono::duration<double>(now-last_).count();
    const auto io=Io(process_);const auto cpu=CpuTicks(process_);
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);GetProcessMemoryInfo(process_,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
    if(pipeline_) {bytes=pipeline_->outputBytesWritten.load();files=pipeline_->outputFilesCreated.load();}
    if(!final)bytes=SelectProgressBytes(basis_,bytes.load(),
      CounterDelta(io.ReadTransferCount,ioStart_.ReadTransferCount),CounterDelta(io.WriteTransferCount,ioStart_.WriteTransferCount),total_);
    const uint64_t current=bytes.load();const auto count=files.load();
    const double speed=interval>0 && current>=lastBytes_?(current-lastBytes_)/MiB/interval:0;
    if(!final || interval>=0.5){peak_=std::max(peak_,speed);hasSpeedSample_=true;}
    peakWorking_=std::max(peakWorking_,memory.WorkingSetSize/MiB);peakPrivate_=std::max(peakPrivate_,memory.PrivateUsage/MiB);
    const double cpuPercent=interval>0?CounterDelta(cpu,cpuLast_)/10000000.0/interval/std::max(1u,std::thread::hardware_concurrency())*100:0;
    const double readRate=interval>0?CounterDelta(io.ReadTransferCount,ioLast_.ReadTransferCount)/MiB/interval:0;
    const double writeRate=interval>0?CounterDelta(io.WriteTransferCount,ioLast_.WriteTransferCount)/MiB/interval:0;
    {std::lock_guard lock(run_.log.mutex);
      run_.log.samples << Stamp() << ',' << name_ << ',' << elapsed << ',' << current << ',' << count << ',' << speed << ',' << cpuPercent << ',' << memory.WorkingSetSize/MiB << ',' << memory.PrivateUsage/MiB << ','
        << readRate << ',' << writeRate << "\r\n";
      run_.log.samples.flush();}
    if(pipelineProgress_)pipelineProgress_->Sample(final);
    const bool child=basis_!=ProgressBasis::LogicalBytes;
    std::string activity=final?"Finished":"";
    if(child && !final) {
      if(readRate>0)activity=basis_==ProgressBasis::ChildInput?"Reading input / compressing":"Extracting";
      else if(writeRate>0)activity="Writing output";
      else if(cpuPercent>0)activity="7-Zip working (CPU active)";
      else activity="Waiting (no measured CPU or I/O activity)";
    }
    if(child && (final || elapsed-lastActivityLog_>=15)) {
      std::ostringstream line;line << "ACTIVITY " << name_ << "; " << activity << "; basis=" << ProgressBasisName(basis_)
        << "; input_bytes=" << CounterDelta(io.ReadTransferCount,ioStart_.ReadTransferCount)
        << "; output_bytes=" << CounterDelta(io.WriteTransferCount,ioStart_.WriteTransferCount)
        << "; read_MiB_s=" << readRate << "; write_MiB_s=" << writeRate << "; cpu_percent=" << cpuPercent
        << "; working_set_MiB=" << memory.WorkingSetSize/MiB;
      run_.log.Line(line.str());lastActivityLog_=elapsed;
    }
    Progress progress{name_,activity,current,total_,count,elapsed,speed,
      total_?static_cast<int>(std::min<uint64_t>(child && !final?99:100,current*100/total_)):0};
    progress.basis=basis_;progress.processReadMiBps=readRate;progress.processWriteMiBps=writeRate;
    progress.cpuPercent=cpuPercent;progress.workingSetMiB=memory.WorkingSetSize/MiB;
    run_.Emit(std::move(progress));
    last_=now;lastBytes_=current;cpuLast_=cpu;ioLast_=io;
  }
  Run& run_;std::string name_;uint64_t total_;HANDLE process_;ProgressBasis basis_;PipelineExtractionDiagnostics* pipeline_;PipelineProgressSnapshot* pipelineProgress_;
  Clock::time_point start_,last_;uint64_t cpuStart_,cpuLast_;IO_COUNTERS ioStart_,ioLast_;
  uint64_t lastBytes_{0};double peak_{0},peakWorking_{0},peakPrivate_{0},lastActivityLog_{0};
  std::mutex waitMutex_;std::condition_variable stop_;bool finished_{false},recorded_{false},hasSpeedSample_{false};std::thread sampler_;
};

struct Drive {
  std::wstring root, identity;
  uint64_t free{0};uint64_t cluster{4096};std::string fileSystem,model,bus;
  int disk{-1};int seekPenalty{-1};
  J ToJson() const {return J(J::Object{{"root",PathValue(root)},{"free_bytes",Number(free)},{"cluster_bytes",Number(cluster)},
    {"filesystem",J(fileSystem)},{"model",J(model)},{"bus",J(bus)},{"physical_disk_number",J(static_cast<double>(disk))},{"seek_penalty",J(static_cast<double>(seekPenalty))}});}
};

Drive DescribeDrive(const std::filesystem::path& parent) {
  Drive drive;wchar_t root[32768]{};
  if(!GetVolumePathNameW(parent.c_str(),root,static_cast<DWORD>(std::size(root))))throw std::runtime_error(ErrorText("Locate volume",parent));
  drive.root=root;wchar_t guid[256]{};
  if(!GetVolumeNameForVolumeMountPointW(root,guid,static_cast<DWORD>(std::size(guid))))throw std::runtime_error("Unable to identify local volume; network/unknown volumes are not supported: "+PathToUtf8(parent));
  drive.identity=guid;
  ULARGE_INTEGER free{},total{};if(!GetDiskFreeSpaceExW(root,&free,&total,nullptr))throw std::runtime_error(ErrorText("Query free space",parent));drive.free=free.QuadPart;
  wchar_t fs[128]{};GetVolumeInformationW(root,nullptr,0,nullptr,nullptr,nullptr,fs,static_cast<DWORD>(std::size(fs)));drive.fileSystem=PathToUtf8(fs);
  DWORD sectors{},bytes{},a{},b{};if(GetDiskFreeSpaceW(root,&sectors,&bytes,&a,&b))drive.cluster=static_cast<uint64_t>(sectors)*bytes;
  if(parent.root_name().wstring().size()==2) {
    Handle volume(CreateFileW((L"\\\\.\\"+parent.root_name().wstring()).c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
    if(volume.value!=INVALID_HANDLE_VALUE) {
      DWORD returned{};STORAGE_DEVICE_NUMBER number{};
      if(DeviceIoControl(volume.value,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),&returned,nullptr))drive.disk=static_cast<int>(number.DeviceNumber);
      STORAGE_PROPERTY_QUERY query{};query.PropertyId=StorageDeviceSeekPenaltyProperty;query.QueryType=PropertyStandardQuery;
      DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};if(DeviceIoControl(volume.value,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),&seek,sizeof(seek),&returned,nullptr))drive.seekPenalty=seek.IncursSeekPenalty?1:0;
      query.PropertyId=StorageDeviceProperty;std::vector<uint8_t> buffer(4096);
      if(DeviceIoControl(volume.value,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),buffer.data(),static_cast<DWORD>(buffer.size()),&returned,nullptr) && returned>=sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* descriptor=reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
        if(descriptor->ProductIdOffset>0 && descriptor->ProductIdOffset<returned) {
          const auto* start=reinterpret_cast<const char*>(buffer.data()+descriptor->ProductIdOffset);const auto* end=reinterpret_cast<const char*>(buffer.data()+returned);
          drive.model.assign(start,std::find(start,end,'\0'));
        }
        switch(descriptor->BusType) {
          case BusTypeNvme: drive.bus="NVMe";break;
          case BusTypeSata: drive.bus="SATA";break;
          case BusTypeUsb: drive.bus="USB";break;
          case BusTypeAta: drive.bus="ATA";break;
          case BusTypeScsi: drive.bus="SCSI";break;
          case BusTypeRAID: drive.bus="RAID";break;
          default: drive.bus="Other("+std::to_string(static_cast<int>(descriptor->BusType))+")";break;
        }
      }
    }
  }
  return drive;
}

std::filesystem::path MakeLogFolder(const std::string& id) {
  auto folder=ModuleFolder()/"logs"/id;std::error_code ec;
  if(!std::filesystem::create_directories(folder,ec) || ec) {
    wchar_t local[32768]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,static_cast<DWORD>(std::size(local))))throw std::runtime_error("No writable log folder");
    folder=std::filesystem::path(local)/"ModlistDiagnostics"/"logs"/id;ec.clear();
    if(!std::filesystem::create_directories(folder,ec) || ec)throw std::runtime_error("Unable to create a unique log folder");
  }
  return folder;
}

void CreateUnique(const std::filesystem::path& path) {
  std::error_code ec;if(!std::filesystem::create_directory(path,ec) || ec)throw std::runtime_error("Unable to exclusively create test folder: "+PathToUtf8(path)+"; "+ec.message());
  std::ofstream marker(path/"diagnostics-owned.txt",std::ios::binary);marker << "Generated benchmark data. Retained; never automatically deleted.\r\n";
  if(!marker)throw std::runtime_error("Unable to write run marker");
}

void PrepareParent(Run& run,const std::filesystem::path& path,const std::string& role) {
  run.Check();
  std::error_code ec;
  const bool created=std::filesystem::create_directories(path,ec);
  if(ec)throw std::runtime_error(ErrorText("Cannot prepare "+role+" folder",path,static_cast<DWORD>(ec.value()))+": "+ec.message());
  if(!std::filesystem::is_directory(path,ec))throw std::runtime_error(ErrorText("Invalid "+role+" folder",path,ec?static_cast<DWORD>(ec.value()):ERROR_DIRECTORY));
  run.log.Line(std::string(created?"Created ":"Using ")+role+" folder: "+PathToUtf8(path));
}

void Generate(Run& run,const std::vector<FileSpec>& specs,const std::filesystem::path& root,bool smallProfile) {
  uint64_t total=0;for(const auto& spec:specs)if(!spec.directory && spec.smallFile==smallProfile)total+=spec.size;
  Stage stage(run,smallProfile?"generate_small":"generate_bulk",total);
  std::vector<uint8_t> buffer(1024*1024);
  size_t index=0;
  for(const auto& spec:specs) {
    ++index;run.Check();if(spec.directory || spec.smallFile!=smallProfile)continue;
    const auto path=root/spec.relative;std::filesystem::create_directories(path.parent_path());
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error(ErrorText("Generate file",path));
    std::mt19937_64 random(0x4d6f646c697374ull+index);uint64_t done=0;
    while(done<spec.size) {
      run.Check();const size_t count=static_cast<size_t>(std::min<uint64_t>(buffer.size(),spec.size-done));
      for(size_t offset=0;offset<count;offset+=8) {
        const uint64_t word=((done+offset)/4096)%2 ? random() : (0x5453494c444f4d31ull ^ static_cast<uint64_t>(index));
        std::memcpy(buffer.data()+offset,&word,std::min<size_t>(8,count-offset));
      }
      DWORD written{};if(!WriteFile(file.value,buffer.data(),static_cast<DWORD>(count),&written,nullptr) || written!=count)throw std::runtime_error(ErrorText("Write generated data",path));
      done+=written;stage.bytes.fetch_add(written);
    }
    if(!smallProfile && spec.size && !FlushFileBuffers(file.value))throw std::runtime_error(ErrorText("Flush generated file",path));
    ++stage.files;
  }
  if(!smallProfile)for(const auto& spec:specs)if(spec.directory)std::filesystem::create_directories(root/spec.relative);
  stage.Finish();
}

std::string HashFile(Run& run,Algorithm& algorithm,const std::filesystem::path& path,Stage* stage=nullptr,
                     std::vector<std::string>* chunks=nullptr) {
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
  if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error(ErrorText("Open for SHA-256",path));
  Hash full(algorithm.handle);std::unique_ptr<Hash> chunk;
  if(chunks)chunk=std::make_unique<Hash>(algorithm.handle);uint64_t chunkBytes=0;
  std::vector<uint8_t> buffer(1024*1024);
  while(true) {
    run.Check();DWORD read{};if(!ReadFile(file.value,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr))throw std::runtime_error(ErrorText("Read for SHA-256",path));
    if(!read)break;full.Add(buffer.data(),read);if(stage)stage->bytes.fetch_add(read);
    if(chunks){chunk->Add(buffer.data(),read);chunkBytes+=read;if(chunkBytes==64ull*1024*1024){chunks->push_back(chunk->Finish());chunk=std::make_unique<Hash>(algorithm.handle);chunkBytes=0;}}
  }
  if(chunks && chunkBytes)chunks->push_back(chunk->Finish());if(stage)++stage->files;return full.Finish();
}

struct CopyContext {Run* run;Stage* stage;uint64_t base;};
DWORD CALLBACK CopyProgress(LARGE_INTEGER total,LARGE_INTEGER transferred,LARGE_INTEGER,LARGE_INTEGER,DWORD,DWORD,HANDLE,HANDLE,LPVOID opaque) {
  auto& context=*static_cast<CopyContext*>(opaque);context.stage->bytes=context.base+static_cast<uint64_t>(transferred.QuadPart);
  (void)total;return context.run->cancel.load()?PROGRESS_CANCEL:PROGRESS_CONTINUE;
}
void CopyProfile(Run& run,const std::vector<FileSpec>& specs,const std::filesystem::path& source,const std::filesystem::path& target,bool smallProfile) {
  uint64_t total=0;for(const auto& spec:specs)if(!spec.directory && spec.smallFile==smallProfile)total+=spec.size;
  Stage stage(run,smallProfile?"copy_small":"copy_bulk",total);
  for(const auto& spec:specs) {
    run.Check();if(spec.directory){if(!smallProfile)std::filesystem::create_directories(target/spec.relative);continue;}
    if(spec.smallFile!=smallProfile)continue;
    const auto path=target/spec.relative;std::filesystem::create_directories(path.parent_path());
    CopyContext context{&run,&stage,stage.bytes.load()};
    if(!CopyFileExW((source/spec.relative).c_str(),path.c_str(),CopyProgress,&context,nullptr,COPY_FILE_FAIL_IF_EXISTS))throw std::runtime_error(ErrorText("Copy failed",path));
    stage.bytes=context.base+spec.size;++stage.files;
  }
  stage.Finish();
}

void VerifyTree(Run& run,const std::vector<FileSpec>& specs,
                const std::filesystem::path& root,const std::string& name) {
  Stage stage(run,name,0);std::set<std::filesystem::path> expected, seen;
  for(const auto& spec:specs) {
    expected.insert(spec.relative);
    for(auto parent=spec.relative.parent_path();!parent.empty();parent=parent.parent_path())expected.insert(parent);
    run.Check();const auto path=root/spec.relative;
    if(spec.directory){if(!std::filesystem::is_directory(path))throw std::runtime_error("Missing directory: "+PathToUtf8(path));}
    else {
      if(!std::filesystem::is_regular_file(path))throw std::runtime_error("Missing regular file: "+PathToUtf8(path));
      if(std::filesystem::file_size(path)!=spec.size)throw std::runtime_error("File size mismatch: "+PathToUtf8(path));
      ++stage.files;
    }
  }
  for(const auto& entry:std::filesystem::recursive_directory_iterator(root)) {
    run.Check();if((GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT)!=0)throw std::runtime_error("Unexpected reparse point in output");
    seen.insert(entry.path().lexically_relative(root));
  }
  if(seen!=expected)throw std::runtime_error("Output inventory mismatch: "+PathToUtf8(root));
  stage.Finish();
}

std::wstring Quote(const std::filesystem::path& path) {
  std::wstring out=L"\"";size_t slashes=0;
  for(wchar_t ch:path.wstring()) {if(ch==L'\\'){++slashes;continue;}if(ch==L'"'){out.append(slashes*2+1,L'\\');out+=ch;}else{out.append(slashes,L'\\');out+=ch;}slashes=0;}
  out.append(slashes*2,L'\\');return out+L'"';
}

void Child(Run& run,const std::filesystem::path& exe,const std::wstring& arguments,const std::filesystem::path& cwd,
           const std::filesystem::path& output,const std::string& stageName,uint64_t bytes,uint64_t files,ProgressBasis basis) {
  SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
  Handle capture(CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
  Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
  Handle job(CreateJobObjectW(nullptr,nullptr));
  if(capture.value==INVALID_HANDLE_VALUE || input.value==INVALID_HANDLE_VALUE || !job.value)throw std::runtime_error(ErrorText("Prepare 7-Zip process",output));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if(!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))throw std::runtime_error("Unable to supervise 7-Zip job");
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=input.value;startup.hStdOutput=capture.value;startup.hStdError=capture.value;
  PROCESS_INFORMATION pi{};std::wstring command=Quote(exe)+L" "+arguments;
  run.log.Line("COMMAND "+PathToUtf8(command)+"; cwd="+PathToUtf8(cwd));
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,cwd.c_str(),&startup,&pi))throw std::runtime_error(ErrorText("Launch 7-Zip",exe));
  Handle process(pi.hProcess),thread(pi.hThread);
  if(!AssignProcessToJobObject(job.value,process.value)) {TerminateProcess(process.value,ERROR_CANCELLED);WaitForSingleObject(process.value,INFINITE);throw std::runtime_error("Unable to attach 7-Zip to supervised job");}
  Stage stage(run,stageName,bytes,process.value,basis);
  if(ResumeThread(thread.value)==static_cast<DWORD>(-1))throw std::runtime_error("Unable to resume 7-Zip");
  while(WaitForSingleObject(process.value,100)==WAIT_TIMEOUT) {
    if(run.cancel.load()){TerminateJobObject(job.value,ERROR_CANCELLED);WaitForSingleObject(process.value,INFINITE);run.Check();}
  }
  DWORD exit{};if(!GetExitCodeProcess(process.value,&exit) || exit!=0)throw std::runtime_error("7-Zip exit code "+std::to_string(exit)+"; details: "+PathToUtf8(output));
  stage.Finish("ok",bytes,files);
}

J PipelineMetrics(const PipelineExtractionDiagnostics& d) {
  J::Object object;
#define METRIC(name) object.emplace(#name,Number(d.name.load()))
  METRIC(inputDiskBytes);METRIC(inputReadOps);METRIC(blockLoads);METRIC(blockReloads);METRIC(cacheHits);METRIC(cacheMisses);
  METRIC(blockWaitNanoseconds);METRIC(diskWaitNanoseconds);METRIC(hashNanoseconds);METRIC(outputFilesCreated);
  METRIC(outputBytesWritten);METRIC(outputWriteCalls);METRIC(outputCreateNanoseconds);METRIC(outputWriteNanoseconds);
  METRIC(outputCloseNanoseconds);METRIC(outputDirectoriesChecked);METRIC(outputDirectoryNanoseconds);
#undef METRIC
  return J(std::move(object));
}

}  // namespace

Outcome RunBenchmark(const Config& config,std::atomic_bool& cancel,ProgressCallback callback) {
  Outcome outcome;J::Object report;std::unique_ptr<Log> log;std::unique_ptr<Run> run;
  try {
    uint64_t nonce{};if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&nonce),sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG))throw std::runtime_error("Unable to create run identity");
    std::string id="ModlistBench-"+Stamp()+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(nonce);
    std::replace(id.begin(),id.end(),':','-');std::replace(id.begin(),id.end(),' ','_');
    outcome.logFolder=MakeLogFolder(id);log=std::make_unique<Log>(outcome.logFolder);
    run=std::make_unique<Run>(Run{config,cancel,std::move(callback),*log,{}});
    log->Line("Modlist diagnostics; logs="+PathToUtf8(outcome.logFolder));
    report={{"schema",J(std::string("modlist-diagnostics-v1"))},{"started",J(Stamp())},
      {"source_parent",PathValue(config.sourceParent)},{"destination_parent",PathValue(config.destinationParent)},
      {"size_gib",Number(config.sizeGiB)},{"smoke",J(config.smoke)},{"threads",Number(config.threads)},
      {"sevenzip_first",J(config.sevenZipFirst)},{"reverse_pass",J(config.reversePass)},
      {"per_file_sha256",J(false)},{"output_verification",J(std::string("paths_types_sizes_only"))},
      {"cache_caveat",J(std::string("Windows caches are not flushed. Generation, copy, compression and verification warm caches. Compare both orders; no cold-cache claim."))},
      {"timing_caveat",J(std::string("Pipeline includes archive SHA-256; ordinary 7-Zip does not. Output checks use metadata only, without payload rereads or per-file SHA-256; same-size content changes are not detected. Metadata stages report file rates, not payload throughput. Thread wait/hash/write durations overlap and must not be summed."))},
      {"peak_caveat",J(std::string("Peak is a roughly one-second sampled rate, not an instantaneous or hardware speed guarantee. Compression progress uses child input-read bytes; native extraction uses output-write bytes. Both are OS-process proxies, not physical disk traffic. Final average uses known logical payload bytes."))}};
    const auto error=ValidateConfig(config);if(!error.empty())throw std::runtime_error(error);run->Check();
    log->Line("FOLDERS source="+PathToUtf8(config.sourceParent)+"; destination="+PathToUtf8(config.destinationParent));
    PrepareParent(*run,config.sourceParent,"source");PrepareParent(*run,config.destinationParent,"destination");
    const uint64_t total=config.smoke?64ull*1024*1024:static_cast<uint64_t>(config.sizeGiB)*GiB;
    const auto specs=BuildWorkload(total,config.smoke);uint64_t fileCount=0;
    for(const auto& spec:specs)if(!spec.directory)++fileCount;
    const auto sourceDrive=DescribeDrive(config.sourceParent),targetDrive=DescribeDrive(config.destinationParent);
    const bool sameVolume=PathTextEqual(sourceDrive.identity,targetDrive.identity);
    report["source_drive"]=sourceDrive.ToJson();report["destination_drive"]=targetDrive.ToJson();
    if(sameVolume && !config.smoke)throw std::runtime_error("Choose folders on different volumes for a cross-disk test");
    if(sameVolume)log->Line("WARNING: smoke test uses the same volume; not a disk-to-disk performance result.");
    if(sourceDrive.disk>=0 && sourceDrive.disk==targetDrive.disk)log->Line("WARNING: source/destination share a physical disk; read/write contention is expected.");
    uint64_t sourceAllocated=0,targetAllocated=0;
    for(const auto& spec:specs)if(!spec.directory){sourceAllocated+=(spec.size+sourceDrive.cluster-1)/sourceDrive.cluster*sourceDrive.cluster;targetAllocated+=(spec.size+targetDrive.cluster-1)/targetDrive.cluster*targetDrive.cluster;}
    const uint64_t sourceRequired=sourceAllocated+total+GiB;
    const uint64_t targetRequired=targetAllocated*(config.reversePass?5:3)+GiB;
    report["source_required_bytes"]=Number(sourceRequired);report["destination_required_bytes"]=Number(targetRequired);
    log->Line("SPACE source_required="+std::to_string(sourceRequired)+"; destination_required="+std::to_string(targetRequired));
    if(sameVolume ? sourceDrive.free<sourceRequired+targetRequired : sourceDrive.free<sourceRequired || targetDrive.free<targetRequired)throw std::runtime_error("Insufficient free space for retained test data and all extraction outputs");
    MEMORYSTATUSEX memory{sizeof(memory)};GlobalMemoryStatusEx(&memory);
    report["system_ram_bytes"]=Number(memory.ullTotalPhys);report["logical_processors"]=Number(std::thread::hardware_concurrency());
    wchar_t cpu[512]{};DWORD cpuBytes=sizeof(cpu);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",L"ProcessorNameString",RRF_RT_REG_SZ,nullptr,cpu,&cpuBytes)==ERROR_SUCCESS)report["cpu_model"]=PathValue(cpu);
    outcome.sourceRun=config.sourceParent/id;outcome.destinationRun=config.destinationParent/id;
    for(const auto& spec:specs)if((outcome.sourceRun/"data"/spec.relative).wstring().size()>240 || (outcome.destinationRun/"pipeline-2"/spec.relative).wstring().size()>240)throw std::runtime_error("Choose shorter parent folders: generated full paths must fit within 240 characters");
    CreateUnique(outcome.sourceRun);CreateUnique(outcome.destinationRun);
    const auto source=outcome.sourceRun/"data";const auto copied=outcome.destinationRun/"copy";const auto archives=outcome.sourceRun/"archive";
    std::filesystem::create_directory(source);std::filesystem::create_directory(copied);std::filesystem::create_directory(archives);
    report["source_run"]=PathValue(outcome.sourceRun);report["destination_run"]=PathValue(outcome.destinationRun);report["payload_bytes"]=Number(total);report["payload_files"]=Number(fileCount);
    log->Line("ENVIRONMENT "+Json(J(report)));
    Generate(*run,specs,source,false);Generate(*run,specs,source,true);
    {std::ofstream inventory(outcome.logFolder/"source-inventory.jsonl",std::ios::binary);for(const auto& spec:specs)inventory << Json(J(J::Object{{"path",PathValue(spec.relative)},{"directory",J(spec.directory)},{"size",Number(spec.size)}})) << '\n';if(!inventory)throw std::runtime_error("Unable to save source inventory");}
    CopyProfile(*run,specs,source,copied,false);CopyProfile(*run,specs,source,copied,true);
    VerifyTree(*run,specs,copied,"verify_copy");
    Algorithm algorithm;
    SevenZipExtractor tools;PipelinedSevenZipExtractor pipeline;
    const auto executable=tools.LocateExecutable(ModuleFolder()),library=pipeline.LocateLibrary(ModuleFolder());
    if(!executable.ok() || !library.ok())throw std::runtime_error("Unable to load embedded 7-Zip tools");
    report["sevenzip_exe_sha256"]=J(HashFile(*run,algorithm,executable.value()));report["sevenzip_dll_sha256"]=J(HashFile(*run,algorithm,library.value()));
    const auto archive=archives/"Benchmark.7z";
    Child(*run,executable.value(),L"a "+Quote(archive)+L" * -t7z -mx=5 -m0=LZMA2 -md=32m -mfb=32 -ms=8g -mmt="+std::to_wstring(config.threads)+L" -v4092m -y -bb0 -bsp0",source,outcome.logFolder/"7zip-create.log","archive_create",total,fileCount,ProgressBasis::ChildInput);
    Manifest manifest;manifest.version="modlist-manifest-chunks-v1";manifest.archiveName="Benchmark";manifest.hashChunkSize=64ull*1024*1024;manifest.unpackedSize=total;
    std::vector<std::filesystem::path> parts;
    for(const auto& entry:std::filesystem::directory_iterator(archives))if(entry.is_regular_file())parts.push_back(entry.path());
    std::sort(parts.begin(),parts.end());uint64_t archiveBytes=0;for(const auto& part:parts)archiveBytes+=std::filesystem::file_size(part);
    if(parts.empty())throw std::runtime_error("7-Zip produced no archive volumes");
    J::Array manifestFiles;
    {Stage stage(*run,"archive_sha256",archiveBytes);for(const auto& part:parts){ManifestFile file;file.path=part.filename();file.size=std::filesystem::file_size(part);file.sha256=HashFile(*run,algorithm,part,&stage,&file.chunkSha256);J::Array chunks;for(const auto& hash:file.chunkSha256)chunks.emplace_back(hash);manifestFiles.emplace_back(J::Object{{"path",PathValue(file.path)},{"size",Number(file.size)},{"sha256",J(file.sha256)},{"chunks",J(std::move(chunks))}});manifest.files.push_back(std::move(file));}stage.Finish();}
    manifest.extract.firstArchivePart=manifest.files.front().path;
    {std::ofstream file(archives/"manifest.json",std::ios::binary);file << Json(J(J::Object{{"schema",J(manifest.version)},{"archive_name",J(manifest.archiveName)},{"unpacked_size",Number(total)},{"hash",J(J::Object{{"algorithm",J(std::string("sha256"))},{"chunk_size",Number(manifest.hashChunkSize)}})},{"files",J(std::move(manifestFiles))}}));if(!file)throw std::runtime_error("Unable to save archive manifest");}
    report["archive_bytes"]=Number(archiveBytes);report["archive_volumes"]=Number(parts.size());J::Array pipelineRecords;
    for(unsigned round=1;round<=(config.reversePass?2u:1u);++round) {
      const bool nativeFirst=config.sevenZipFirst^(round==2);
      for(int index=0;index<2;++index) {
        run->Check();const bool native=(index==0)==nativeFirst;const std::string kind=native?"7zip":"pipeline";
        const auto suffix=kind+"-"+std::to_string(round);const auto output=outcome.destinationRun/suffix;
        if(!std::filesystem::create_directory(output))throw std::runtime_error("Extraction output already exists");
        log->Line("ORDER round="+std::to_string(round)+"; position="+std::to_string(index+1)+"; extractor="+kind);
        if(native)Child(*run,executable.value(),L"x "+Quote(parts.front())+L" -o"+Quote(output)+L" -y -aoa -mmt="+std::to_wstring(config.threads)+L" -bb0 -bsp0",archives,outcome.logFolder/(suffix+".log"),"extract_"+suffix,total,fileCount,ProgressBasis::ChildOutput);
        else {
          PipelineExtractionDiagnostics metrics;PipelinedExtractionConfig extraction;extraction.sevenZipLibrary=library.value();extraction.archiveFolder=archives;extraction.installFolder=output;extraction.manifest=&manifest;extraction.cancelRequested=&cancel;extraction.decoderThreads=config.threads;extraction.diagnostics=&metrics;
          PipelineProgressSnapshot progressLog([&](const auto& line){log->Line(line);});
          Stage stage(*run,"extract_"+suffix,total,GetCurrentProcess(),ProgressBasis::LogicalBytes,&metrics,&progressLog);
          const auto result=pipeline.Extract(extraction,[&](const auto& progress){
            progressLog.Publish(progress);
          });
          pipelineRecords.emplace_back(J::Object{{"round",Number(round)},{"counters",PipelineMetrics(metrics)}});
          report["pipeline_diagnostics"]=J(pipelineRecords);
          if(!result.ok)throw std::runtime_error("Pipeline extraction failed: "+result.message);
          stage.Finish("ok",total,fileCount);
        }
        VerifyTree(*run,specs,output,"verify_"+suffix);
      }
    }
    report["pipeline_diagnostics"]=J(std::move(pipelineRecords));run->Check();outcome.ok=true;
    log->Line("SUCCESS: copied/extracted trees passed path, type, count and size checks. Per-file SHA-256 disabled; same-size content changes are not detected. Generated data retained.");
  } catch(const std::exception& ex) {
    outcome.cancelled=cancel.load();outcome.error=ex.what();if(log)log->Line(std::string(outcome.cancelled?"CANCELLED: ":"FAILED: ")+outcome.error);
  }
  report["ok"]=J(outcome.ok);report["cancelled"]=J(outcome.cancelled);report["error"]=J(outcome.error);report["finished"]=J(Stamp());
  if(run)report["stages"]=J(run->stages);
  if(!outcome.logFolder.empty()) {
    std::ofstream text(outcome.logFolder/"results.txt",std::ios::binary);text << "Modlist Diagnostics\r\nStatus: " << (outcome.ok?"PASSED":outcome.cancelled?"CANCELLED":"FAILED") << "\r\nError: " << outcome.error << "\r\nSource data: " << PathToUtf8(outcome.sourceRun) << "\r\nDestination data: " << PathToUtf8(outcome.destinationRun) << "\r\n\r\n";
    text.imbue(std::locale::classic());text << std::fixed << std::setprecision(2);
    text << std::left << std::setw(30) << "Stage" << std::right << std::setw(12) << "Seconds"
         << std::setw(14) << "Avg MiB/s" << std::setw(14) << "Peak MiB/s" << std::setw(14) << "Files/sec" << "  Status\r\n";
    if(run)for(const auto& stage:run->stages) {
      text << std::left << std::setw(30) << stage.Find("name")->AsString()
        << std::right << std::setw(12) << stage.Find("seconds")->AsNumber()
        << std::setw(14) << stage.Find("average_mib_s")->AsNumber();
      const auto* peak=stage.Find("peak_sampled_mib_s");
      if(peak->IsNumber())text << std::setw(14) << peak->AsNumber();else text << std::setw(14) << "n/a";
      text << std::setw(14) << stage.Find("files_per_second")->AsNumber()
        << "  " << stage.Find("status")->AsString() << "\r\n";
    }
    text << "\r\nNo cold-cache claim. Peak is a sampled rate; compression tracks input reads and native extraction tracks output writes.\r\n"
         << "These are process-I/O proxies, not physical disk traffic. CPU/RAM and separate read/write rates show activity between output writes.\r\n"
         << "Per-file SHA-256 disabled. Output checks use names, types and sizes only, without payload rereads.\r\n"
         << "Same-size content changes are not detected. Metadata checks report file rates, not payload throughput.\r\n"
         << "Pipeline archive SHA-256 remains enabled; output metadata checks are separately timed.\r\n"
         << "See results.json for pipeline wait/hash/write counters, and samples.csv for per-second CPU/RAM/I/O.\r\n"
         << "All generated data is retained. No existing user files are deleted or overwritten.\r\n";
    text.flush();
    if(!text) {
      outcome.ok=false;outcome.error="Unable to finish text result report";
      report["ok"]=J(false);report["error"]=J(outcome.error);
    }
    std::ofstream summary(outcome.logFolder/"results.json",std::ios::binary);summary << Json(J(report));summary.flush();
    if(!summary){outcome.ok=false;outcome.error="Unable to finish JSON result report";text << "\r\nFinal status: FAILED - " << outcome.error << "\r\n";text.flush();}
  }
  return outcome;
}

}  // namespace modlist::diagnostics
