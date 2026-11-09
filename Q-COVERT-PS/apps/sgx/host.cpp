#include <cstdint>
#include <cerrno>
#include <iostream>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <sys/resource.h>
#include "qcovert/snapshot_file.hpp"
#ifdef BISON_SGX_HOST_MODE
#include "bison/progress.hpp"
#elif defined(COMPASS_SGX_HOST_MODE)
#include "compass/progress.hpp"
#endif

#include <sgx_urts.h>

#include "ringdoram_sgx_u.h"

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/common/threadpool_services.hpp"
#include "sonic/sgxbridge/host/threadpool_manager.hpp"
#include "sonic/threads/tuning.hpp"
#include "sonic/util/log.hpp"

extern "C" void qcovert_benchmark_time_ns(std::uint64_t* nanoseconds) {
  *nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
namespace {
// Shared app_context payload: snapshot image plus the report stream the
// enclave appends one block to after every finished (nprobe, k2) job.
struct app_state {
  qcovert::snapshot::file_transport* image = nullptr;
  std::ostream* report = nullptr;
};
app_state* state_of(void* host) {
  auto* context = static_cast<sn::sgxbridge::common::host_execution_context*>(host);
  return static_cast<app_state*>(context->app_context);
}
} // namespace
extern "C" void qcovert_benchmark_progress(void*, const char* stage, std::uint64_t completed,
                                            std::uint64_t total) {
#ifdef BISON_SGX_HOST_MODE
  bison::host_progress().update(stage, completed, total);
#elif defined(COMPASS_SGX_HOST_MODE)
  compass::host_progress().update(stage, completed, total);
#else
  std::fprintf(stderr, "QCOVERT_PROGRESS\t%s\t%llu\t%llu\n", stage,
               static_cast<unsigned long long>(completed), static_cast<unsigned long long>(total));
  std::fflush(stderr);
#endif
}
extern "C" std::size_t qcovert_snapshot_read(void* host, std::uint8_t* bytes, std::size_t size) {
  try {
    auto* image = state_of(host)->image;
    return image ? image->read(bytes, size) : 0;
  } catch (...) { return 0; }
}
extern "C" std::uint32_t qcovert_snapshot_write(void* host, const std::uint8_t* bytes, std::size_t size) {
  try {
    auto* image = state_of(host)->image;
    if (!image) return 0;
    image->write(bytes, size);
    return 1;
  } catch (...) { return 0; }
}
extern "C" std::uint32_t qcovert_snapshot_commit(void* host) {
  try {
    auto* image = state_of(host)->image;
    if (!image) return 0;
    image->commit();
    return 1;
  } catch (...) { return 0; }
}
extern "C" std::uint32_t qcovert_report_write(void* host, const std::uint8_t* bytes, std::size_t size) {
  try {
    auto* report = state_of(host)->report;
    if (!report) return 0;
    report->write(reinterpret_cast<const char*>(bytes), size);
    report->flush();
    return *report ? 1 : 0;
  } catch (...) { return 0; }
}

namespace {

constexpr const char* default_enclave = "ringdoram_enclave.signed.so";

int run(const char* enclave_path, const char* input_path=nullptr, const char* output_path=nullptr,
        const char* snapshot_path=nullptr) {
  sgx_enclave_id_t enclave_id = 0;
  sgx_launch_token_t token{};
  int token_updated = 0;
  const sgx_status_t create_status = sgx_create_enclave(enclave_path, 1, &token, &token_updated, &enclave_id, nullptr);
  if (create_status != SGX_SUCCESS) {
    std::fprintf(stderr, "sgx_create_enclave failed: 0x%08x (%s)\n", static_cast<unsigned>(create_status), enclave_path);
    return 1;
  }

  int exit_code = 0;
  {
    auto logger = sn::util::log::create("ringdoram.sgx.host");
    sn::threads::thread_context threads;
    sn::sgxbridge::host::threadpool_manager manager;
    manager.configure(enclave_id, threads, &logger);
    sn::sgxbridge::tp::threadpool_services services{&manager};
    sn::sgxbridge::common::host_execution_context host_context{};
    host_context.threadpools = &services;
    host_context.logger = &logger;
    std::unique_ptr<qcovert::snapshot::file_transport> image;
    app_state state;
    if (snapshot_path) {
      image = std::make_unique<qcovert::snapshot::file_transport>(snapshot_path);
      state.image = image.get();
    }
    host_context.app_context = &state;

    std::uint32_t enclave_result = 255;
    sgx_status_t ecall_status;
    if(input_path) {
      std::ifstream in(input_path,std::ios::binary|std::ios::ate);
      if(!in)throw std::runtime_error("cannot open benchmark request");
      auto size=in.tellg();if(size<0)throw std::runtime_error("bad request size");
      std::vector<std::uint8_t> input(static_cast<std::size_t>(size));in.seekg(0);
      if(!in.read(reinterpret_cast<char*>(input.data()),input.size()))throw std::runtime_error("cannot read request");
      // Public protocol offsets differ between the independent baselines.
      if(input.size()<152)throw std::runtime_error("truncated request");
      std::uint64_t count=0,repeats=0;
#ifdef COMPASS_SGX_HOST_MODE
      std::memcpy(&count,input.data()+12*8,8);std::memcpy(&repeats,input.data()+13*8,8);
#else
      std::memcpy(&count,input.data()+14*8,8);std::memcpy(&repeats,input.data()+15*8,8);
#endif
      if(!count||!repeats||repeats>10000||count>1000000/repeats)throw std::runtime_error("report size limit");
#ifdef BISON_SGX_HOST_MODE
      if(input.size()<176)throw std::runtime_error("truncated BiSON request");
      std::uint64_t k=0,jobs=0;std::memcpy(&k,input.data()+4*8,8);std::memcpy(&jobs,input.data()+168,8);
      if(!k||k>65536||!jobs||jobs>1024||jobs>(input.size()-176)/24||count>(1000000/jobs)/repeats)
        throw std::runtime_error("BiSON report/job limit");
      const auto frame_bytes=112+count*repeats*(72+k*16);
      if(frame_bytes>SIZE_MAX/jobs)throw std::runtime_error("BiSON report size overflow");
      std::vector<std::uint8_t> output(jobs*frame_bytes);std::size_t written=0;
#elif defined(COMPASS_SGX_HOST_MODE)
      std::uint64_t k=0;std::memcpy(&k,input.data()+4*8,8);
      if(!k || k>65536)throw std::runtime_error("Compass report k limit");
      if(input.size()<176)throw std::runtime_error("truncated Compass request");
      std::uint64_t jobs=0;std::memcpy(&jobs,input.data()+168,8);
      if(!jobs || jobs>1024 || jobs>(input.size()-176)/8 || count>(1000000/jobs)/repeats)
        throw std::runtime_error("Compass ef job/report size limit");
      const auto frame_bytes=144+count*repeats*(72+k*16);
      if(frame_bytes>SIZE_MAX/jobs)throw std::runtime_error("Compass report size overflow");
      std::vector<std::uint8_t> output(jobs*frame_bytes);std::size_t written=0;
#else
      std::vector<std::uint8_t> output(16384+count*repeats*1024);std::size_t written=0;
#endif
      std::uint32_t image_status=0;
      std::ofstream report_out(output_path, std::ios::binary | std::ios::trunc);
      if (!report_out) throw std::runtime_error("cannot create report file");
      state.report = &report_out;
#ifdef BISON_SGX_HOST_MODE
      ecall_status=bison_sgx_benchmark(enclave_id,&enclave_result,&host_context,input.data(),input.size(),output.data(),output.size(),&written,&image_status);
      bison::host_progress().finish();
#elif defined(COMPASS_SGX_HOST_MODE)
      ecall_status=compass_sgx_benchmark(enclave_id,&enclave_result,&host_context,input.data(),input.size(),output.data(),output.size(),&written,&image_status);
      compass::host_progress().finish();
#else
      ecall_status=qcovert_sgx_benchmark(enclave_id,&enclave_result,&host_context,input.data(),input.size(),output.data(),output.size(),&written,&image_status);
#endif
      if(ecall_status==SGX_SUCCESS && enclave_result==0) {
        if(written>output.size())throw std::runtime_error("invalid report length");
        // Newer enclaves stream each job's report block via qcovert_report_write;
        // written==0 there. Older enclaves still return the whole report at once.
        if(written && !report_out.write(reinterpret_cast<const char*>(output.data()),written))
          throw std::runtime_error("cannot write report");
        report_out.flush();
        std::printf("oram_snapshot_status=%s\n",image_status==1?"loaded":image_status==2?"built":"none");
      }
    } else ecall_status=ringdoram_sgx_run(enclave_id, &enclave_result, &host_context);
    if (ecall_status != SGX_SUCCESS || enclave_result != 0) {
      std::fprintf(stderr, "RingDORAM SGX smoke failed: ecall=0x%08x result=%u\n",
                   static_cast<unsigned>(ecall_status), enclave_result);
      exit_code = 2;
    } else {
      if(input_path) {
        rusage usage{};getrusage(RUSAGE_SELF,&usage);
        std::printf("sgx_mode=%s\npeak_host_rss_bytes=%llu\n",QCOVERT_SGX_MODE,static_cast<unsigned long long>(usage.ru_maxrss)*1024);
      } else std::puts("RingDORAM SGX batch smoke passed");
    }
    manager.shutdown();
  }

  sgx_destroy_enclave(enclave_id);
  return exit_code;
}

constexpr std::uint64_t setup_request_magic = 0x5143554552545301ULL;

struct setup_config {
  std::string label;
  std::uint64_t n = 0, block_bytes = 0, routing_depth = 0;
};

// In-enclave Setup benchmark (bulk_setup / sequential insert). Each finished
// config×path streams one CSV row from the enclave. Usage:
//   host <enclave> --setup --z Z --s S --a A [--evict-batch E] [--threads T]
//        [--paths bulk,sequential] [--csv FILE] label,N,block_bytes,R [...]
int run_setup(const char* enclave_path, const std::vector<std::uint64_t>& request, const std::string& csv_path) {
  sgx_enclave_id_t enclave_id = 0;
  sgx_launch_token_t token{};
  int token_updated = 0;
  const sgx_status_t create_status = sgx_create_enclave(enclave_path, 1, &token, &token_updated, &enclave_id, nullptr);
  if (create_status != SGX_SUCCESS) {
    std::fprintf(stderr, "sgx_create_enclave failed: 0x%08x (%s)\n", static_cast<unsigned>(create_status), enclave_path);
    return 1;
  }
  int exit_code = 0;
  {
    auto logger = sn::util::log::create("ringdoram.sgx.host");
    sn::threads::thread_context threads_context;
    sn::sgxbridge::host::threadpool_manager manager;
    manager.configure(enclave_id, threads_context, &logger);
    sn::sgxbridge::tp::threadpool_services services{&manager};
    sn::sgxbridge::common::host_execution_context host_context{};
    host_context.threadpools = &services;
    host_context.logger = &logger;
    app_state state;
    std::ofstream rows;
    constexpr const char* header = "label,env,path,N,block_bytes,Z,S,A,routing_depth,evict_batch,threads,setup_ms,leaves,stash_bound,storage,epoch,eviction";
    if (!csv_path.empty()) {
      std::ifstream existing(csv_path);
      std::string old_header;
      const bool has_rows = static_cast<bool>(std::getline(existing, old_header));
      if (has_rows && old_header != header) throw std::invalid_argument("setup CSV schema differs; use a new file");
      rows.open(csv_path, std::ios::app);
      if (!rows) throw std::runtime_error("cannot open CSV output");
      if (!has_rows)
        rows << header << '\n';
      state.report = &rows;
    } else {
      std::cout << header << '\n';
      state.report = &std::cout;
    }
    host_context.app_context = &state;

    std::uint8_t dummy = 0;
    std::uint32_t enclave_result = 255;
    const sgx_status_t ecall_status =
        qcovert_sgx_setup(enclave_id, &enclave_result, &host_context,
                          reinterpret_cast<const std::uint8_t*>(request.data()), request.size() * 8,
                          &dummy, sizeof(dummy));
    if (ecall_status != SGX_SUCCESS || enclave_result != 0) {
      std::fprintf(stderr, "RingDORAM SGX setup benchmark failed: ecall=0x%08x result=%u\n",
                   static_cast<unsigned>(ecall_status), enclave_result);
      exit_code = 2;
    }
  }
  sgx_destroy_enclave(enclave_id);
  return exit_code;
}

constexpr std::uint64_t evict_request_magic = 0x5143455643540001ULL;

// In-enclave batched-eviction benchmark (pure evict<> timing for the sonic and
// shared-top backends). Each finished config streams one CSV row from the
// enclave. Usage:
//   host <enclave> --evict [--z Z --s S --a A] [--stash-bound CS] [--csv FILE]
//        label,N,E,sonic|shared,reps [...]
int run_evict(const char* enclave_path, const std::vector<std::uint64_t>& request,
              const std::string& csv_path) {
  sgx_enclave_id_t enclave_id = 0;
  sgx_launch_token_t token{};
  int token_updated = 0;
  const sgx_status_t create_status = sgx_create_enclave(enclave_path, 1, &token, &token_updated, &enclave_id, nullptr);
  if (create_status != SGX_SUCCESS) {
    std::fprintf(stderr, "sgx_create_enclave failed: 0x%08x (%s)\n", static_cast<unsigned>(create_status), enclave_path);
    return 1;
  }
  int exit_code = 0;
  {
    auto logger = sn::util::log::create("ringdoram.sgx.host");
    sn::threads::thread_context threads_context;
    sn::sgxbridge::host::threadpool_manager manager;
    manager.configure(enclave_id, threads_context, &logger);
    sn::sgxbridge::tp::threadpool_services services{&manager};
    sn::sgxbridge::common::host_execution_context host_context{};
    host_context.threadpools = &services;
    host_context.logger = &logger;
    app_state state;
    std::ofstream rows;
    constexpr const char* header =
        "label,env,backend,N,E,L,Cs,repetitions,evict_median_ms,evict_p95_ms,evict_min_ms";
    if (!csv_path.empty()) {
      std::ifstream existing(csv_path);
      std::string old_header;
      const bool has_rows = static_cast<bool>(std::getline(existing, old_header));
      if (has_rows && old_header != header) throw std::invalid_argument("evict CSV schema differs; use a new file");
      rows.open(csv_path, std::ios::app);
      if (!rows) throw std::runtime_error("cannot open CSV output");
      if (!has_rows)
        rows << header << '\n';
      state.report = &rows;
    } else {
      std::cout << header << '\n';
      state.report = &std::cout;
    }
    host_context.app_context = &state;

    std::uint8_t dummy = 0;
    std::uint32_t enclave_result = 255;
    const sgx_status_t ecall_status =
        qcovert_sgx_evict(enclave_id, &enclave_result, &host_context,
                          reinterpret_cast<const std::uint8_t*>(request.data()), request.size() * 8,
                          &dummy, sizeof(dummy));
    if (ecall_status != SGX_SUCCESS || enclave_result != 0) {
      std::fprintf(stderr, "RingDORAM SGX evict benchmark failed: ecall=0x%08x result=%u\n",
                   static_cast<unsigned>(ecall_status), enclave_result);
      exit_code = 2;
    }
  }
  sgx_destroy_enclave(enclave_id);
  return exit_code;
}

} // namespace

int main(int argc, char** argv) {
 try {
#ifdef BISON_SGX_HOST_MODE
  if(argc==2 && std::strcmp(argv[1],"--describe-layout")==0) {
    std::printf("{\"dimension\":%d,\"degree\":%d,\"pq_subspaces\":%d,\"pq_centroids\":256,\"scalar\":\"%s\",\"mode\":\"sgx-%s\",\"snapshot_protocol\":1,\"search_protocol\":2,\"prepared_protocol\":1}\n",
      BISON_DIM,BISON_DEGREE,BISON_PQ_BYTES,BISON_SCALAR_ID==2?"uint8":"float32",QCOVERT_SGX_MODE);
    return 0;
  }
#endif
#ifdef COMPASS_SGX_HOST_MODE
  if(argc==2 && std::strcmp(argv[1],"--describe-layout")==0) {
    std::printf("{\"dimension\":%d,\"degree\":%d,\"pq_subspaces\":%d,\"pq_centroids\":%d,\"scalar\":\"%s\",\"mode\":\"sgx-%s\",\"snapshot_protocol\":4,\"search_protocol\":5,\"prepared_protocol\":3}\n",
      COMPASS_DIM,COMPASS_DEGREE,COMPASS_PQ_SUBSPACES,COMPASS_PQ_CENTROIDS,
      COMPASS_SCALAR_ID==2?"uint8":"float32",QCOVERT_SGX_MODE);
    return 0;
  }
#endif
  if(argc>=3 && std::strcmp(argv[2],"--setup")==0) {
    std::uint64_t z=32,s=58,a=46,evict_batch=1,threads=8,path_mask=3;
    std::string csv_path;
    std::vector<setup_config> configs;
    auto parse_u64=[](const char* v, bool allow_zero=false)->std::uint64_t {
      if (*v == '-') throw std::invalid_argument("expected nonnegative integer");
      errno = 0;
      char* end=nullptr; const auto r=std::strtoull(v,&end,10);
      if(end==v||*end!='\0'||errno==ERANGE||(!allow_zero&&!r))throw std::invalid_argument("expected a positive integer");
      return r;
    };
    for(int i=3;i<argc;++i) {
      const std::string arg=argv[i];
      auto take=[&](const char* name)->const char* {
        if(i+1>=argc)throw std::invalid_argument(std::string("missing value for ")+name);
        return argv[++i];
      };
      if(arg=="--z")z=parse_u64(take("--z"));
      else if(arg=="--s")s=parse_u64(take("--s"));
      else if(arg=="--a")a=parse_u64(take("--a"));
      else if(arg=="--evict-batch")evict_batch=parse_u64(take("--evict-batch"));
      else if(arg=="--threads")threads=parse_u64(take("--threads"));
      else if(arg=="--csv")csv_path=take("--csv");
      else if(arg=="--paths") {
        const std::string v=take("--paths");
        if(v=="bulk")path_mask=1;
        else if(v=="sequential")path_mask=2;
        else if(v=="bulk,sequential"||v=="sequential,bulk")path_mask=3;
        else throw std::invalid_argument("--paths must be bulk, sequential or bulk,sequential");
      } else {
        setup_config c;
        std::string spec=arg;
        std::vector<std::string> parts;
        for(std::size_t at=0,nx=std::string::npos;;at=nx+1) {
          nx=spec.find(',',at);
          parts.push_back(spec.substr(at,nx==std::string::npos?nx:nx-at));
          if(nx==std::string::npos)break;
        }
        if(parts.size()!=4)throw std::invalid_argument("config must be label,N,block_bytes,routing_depth: "+spec);
        c.label=parts[0];
        c.n=parse_u64(parts[1].c_str());
        c.block_bytes=parse_u64(parts[2].c_str());
        c.routing_depth=parse_u64(parts[3].c_str(),true);
        configs.push_back(std::move(c));
      }
    }
    if(configs.empty() || configs.size()>64)throw std::invalid_argument("expected 1..64 setup configs");
    if(z>UINT32_MAX || s>UINT32_MAX-z || a>UINT32_MAX || evict_batch>UINT32_MAX || threads>256)
      throw std::invalid_argument("setup parameters exceed supported range");
    for(const auto& c:configs) {
      if(c.label.empty() || c.label.find_first_of(",\r\n")!=std::string::npos ||
         c.routing_depth>=32 || c.n>INT64_MAX ||
         (c.block_bytes!=64 && c.block_bytes!=128 && c.block_bytes!=1280))
        throw std::invalid_argument("invalid setup config");
    }
    std::vector<std::uint64_t> request{setup_request_magic,static_cast<std::uint64_t>(configs.size()),z,s,a,evict_batch,threads};
    for(const auto& c:configs) {
      request.insert(request.end(),{c.block_bytes,c.n,c.routing_depth,path_mask});
      char label[64]{};
      if(c.label.size()>=sizeof(label))throw std::invalid_argument("setup label too long: "+c.label);
      std::memcpy(label,c.label.data(),c.label.size());
      std::uint64_t words[8]{};
      std::memcpy(words,label,sizeof(label));
      request.insert(request.end(),words,words+8);
    }
    return run_setup(argv[1],request,csv_path);
  }
  if(argc>=3 && std::strcmp(argv[2],"--evict")==0) {
    std::uint64_t z=8,s=8,a=8,stash_bound=0;
    std::string csv_path;
    struct evict_config { std::string label; std::uint64_t n,e,backend,reps; };
    std::vector<evict_config> configs;
    auto parse_u64=[](const char* v, bool allow_zero=false)->std::uint64_t {
      if (*v == '-') throw std::invalid_argument("expected nonnegative integer");
      errno = 0;
      char* end=nullptr; const auto r=std::strtoull(v,&end,10);
      if(end==v||*end!='\0'||errno==ERANGE||(!allow_zero&&!r))throw std::invalid_argument("expected a positive integer");
      return r;
    };
    for(int i=3;i<argc;++i) {
      const std::string arg=argv[i];
      auto take=[&](const char* name)->const char* {
        if(i+1>=argc)throw std::invalid_argument(std::string("missing value for ")+name);
        return argv[++i];
      };
      if(arg=="--z")z=parse_u64(take("--z"));
      else if(arg=="--s")s=parse_u64(take("--s"));
      else if(arg=="--a")a=parse_u64(take("--a"));
      else if(arg=="--stash-bound")stash_bound=parse_u64(take("--stash-bound"),true);
      else if(arg=="--csv")csv_path=take("--csv");
      else {
        evict_config c;
        std::string spec=arg;
        std::vector<std::string> parts;
        for(std::size_t at=0,nx=std::string::npos;;at=nx+1) {
          nx=spec.find(',',at);
          parts.push_back(spec.substr(at,nx==std::string::npos?nx:nx-at));
          if(nx==std::string::npos)break;
        }
        if(parts.size()!=5)throw std::invalid_argument("config must be label,N,E,sonic|shared,reps: "+spec);
        c.label=parts[0];
        c.n=parse_u64(parts[1].c_str());
        c.e=parse_u64(parts[2].c_str());
        if(parts[3]=="sonic")c.backend=0;
        else if(parts[3]=="shared")c.backend=1;
        else throw std::invalid_argument("backend must be sonic or shared: "+spec);
        c.reps=parse_u64(parts[4].c_str());
        configs.push_back(std::move(c));
      }
    }
    if(configs.empty() || configs.size()>64)throw std::invalid_argument("expected 1..64 evict configs");
    if(z>UINT32_MAX || s>UINT32_MAX-z || a>UINT32_MAX)
      throw std::invalid_argument("evict parameters exceed supported range");
    for(const auto& c:configs) {
      if(c.label.empty() || c.label.find_first_of(",\r\n")!=std::string::npos ||
         c.n>INT64_MAX || c.e>UINT32_MAX || c.reps>10000)
        throw std::invalid_argument("invalid evict config");
    }
    std::vector<std::uint64_t> request{evict_request_magic,static_cast<std::uint64_t>(configs.size()),z,s,a,stash_bound};
    for(const auto& c:configs) {
      request.insert(request.end(),{c.n,c.e,c.backend,c.reps});
      char label[64]{};
      if(c.label.size()>=sizeof(label))throw std::invalid_argument("evict label too long: "+c.label);
      std::memcpy(label,c.label.data(),c.label.size());
      std::uint64_t words[8]{};
      std::memcpy(words,label,sizeof(label));
      request.insert(request.end(),words,words+8);
    }
    return run_evict(argv[1],request,csv_path);
  }
  if(argc==4 || argc==5)return run(argv[1],argv[2],argv[3],argc==5?argv[4]:nullptr);
  if(argc>2)throw std::invalid_argument("usage: host [enclave] [benchmark-input benchmark-output [initial.state]]");
  return run(argc>1?argv[1]:default_enclave);
 }catch(const std::exception& e){
#ifdef BISON_SGX_HOST_MODE
  bison::host_progress().finish();
#elif defined(COMPASS_SGX_HOST_MODE)
  compass::host_progress().finish();
#endif
  std::fprintf(stderr,"%s\n",e.what());return 1;
 }
}
