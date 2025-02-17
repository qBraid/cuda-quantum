#include "common/Logger.h"
#include "common/RestClient.h"
#include "common/ServerHelper.h"
#include "cudaq/Support/Version.h"
#include "cudaq/utils/cudaq_utils.h"
#include <bitset>
#include <fstream>
#include <map>
#include <thread>

namespace cudaq {

class QbraidServerHelper : public ServerHelper {
  static constexpr const char *DEFAULT_URL = "https://api.qbraid.com/api";
  static constexpr const char *DEFAULT_DEVICE = "qbraid_qir_simulator";
  static constexpr int DEFAULT_QUBITS = 29;  // Added default qubit count

public:
  const std::string name() const override { return "qbraid"; }

  void initialize(BackendConfig config) override {
    cudaq::info("Initializing Qbraid Backend.");
    
    // Initialize required configuration
    backendConfig.clear(); 
    backendConfig["url"] = getValueOrDefault(config, "url", DEFAULT_URL);
    backendConfig["device_id"] = getValueOrDefault(config, "device_id", DEFAULT_DEVICE);
    backendConfig["user_agent"] = "cudaq/" + std::string(cudaq::getVersion());
    backendConfig["qubits"] = std::to_string(DEFAULT_QUBITS);  // Set default qubits
    
    // Get API key from environment
    backendConfig["api_key"] = getEnvVar("QBRAID_API_KEY", "", true);
    
    // Job endpoints
    backendConfig["job_path"] = backendConfig["url"] + "/quantum-jobs";
    
    // Set shots if provided
    if (!config["shots"].empty()) {
      backendConfig["shots"] = config["shots"];
      this->setShots(std::stoul(config["shots"]));
    } else {
      backendConfig["shots"] = "1000";  // Default shots
      this->setShots(1000);
    }

    parseConfigForCommonParams(config);

    cudaq::info("Qbraid configuration initialized:");
    for (const auto& [key, value] : backendConfig) {
      cudaq::info("  {} = {}", key, value);
    }
  }

  ServerJobPayload createJob(std::vector<KernelExecution> &circuitCodes) override {
    if (backendConfig.find("job_path") == backendConfig.end()) {
      throw std::runtime_error("job_path not found in config. Was initialize() called?");
    }

    std::vector<ServerMessage> jobs;
    for (auto &circuitCode : circuitCodes) {
      ServerMessage job;
      job["qbraidDeviceId"] = backendConfig.at("device_id");
      job["bitcode"] = circuitCode.code;  
      job["shots"] = std::stoi(backendConfig.at("shots"));
      job["circuitNumQubits"] = std::stoi(backendConfig.at("qubits"));
      
      if (!circuitCode.name.empty()) {
        nlohmann::json tags;
        tags["name"] = circuitCode.name;
        job["tags"] = tags;
      }
      
      jobs.push_back(job);
    }

    return std::make_tuple(backendConfig.at("job_path"), getHeaders(), jobs);
  }

  std::string extractJobId(ServerMessage &postResponse) override {
    if (!postResponse.contains("qbraidJobId"))
      throw std::runtime_error("ServerMessage doesn't contain 'qbraidJobId' key.");
    return postResponse.at("qbraidJobId");
  }

  std::string constructGetJobPath(ServerMessage &postResponse) override {
    if (!postResponse.contains("qbraidJobId"))
      throw std::runtime_error("ServerMessage doesn't contain 'qbraidJobId' key.");
    return backendConfig.at("job_path") + "?vendorJobId=" + 
           postResponse.at("qbraidJobId").get<std::string>();
  }

  std::string constructGetJobPath(std::string &jobId) override {
    return backendConfig.at("job_path") + "?vendorJobId=" + jobId;
  }

  bool jobIsDone(ServerMessage &getJobResponse) override {
    if (!getJobResponse.contains("jobsArray") || getJobResponse["jobsArray"].empty()) {
      cudaq::info("Invalid job response format: {}", getJobResponse.dump());
      throw std::runtime_error("Invalid job response format");
    }

    auto status = getJobResponse["jobsArray"][0]["status"].get<std::string>();
    cudaq::info("Job status: {}", status);
    
    if (status == "FAILED")
      throw std::runtime_error("The job failed upon submission. Check your qBraid account for more information.");
    
    return status == "COMPLETED";
  }

cudaq::sample_result processResults(ServerMessage &getJobResponse,
                                  std::string &jobId) override {

    cudaq::info("Processing results for job {}", jobId);
    cudaq::info("Full response: {}", getJobResponse.dump(2));

    // if (!getJobResponse.contains("jobsArray")) {
    //     cudaq::info("Response missing jobsArray field");
    //     throw std::runtime_error("Invalid job response format: missing jobsArray");
    // }

    // if (getJobResponse["jobsArray"].empty()) {
    //     cudaq::info("jobsArray is empty");
    //     throw std::runtime_error("Invalid job response format: empty jobsArray");
    // }

    auto &job = getJobResponse["jobsArray"][0];
    
    cudaq::info("Job fields available: ");
    for (const auto& [key, value] : job.items()) {
        cudaq::info("  {} = {}", key, value.dump());
    }

    if (!job.contains("measurementCounts")) {
        cudaq::info("Job response missing measurementCounts field. Available fields:");
        for (const auto& [key, value] : job.items()) {
            cudaq::info("  {}", key);
        }
        
        if (job.contains("results") || job.contains("counts") || job.contains("measurements")) {
            cudaq::info("Found alternative results field!");

            if (job.contains("results")) cudaq::info("results field: {}", job["results"].dump());
            if (job.contains("counts")) cudaq::info("counts field: {}", job["counts"].dump());
            if (job.contains("measurements")) cudaq::info("measurements field: {}", job["measurements"].dump());
        }
        
        throw std::runtime_error("No measurement counts in response");
    }

    CountsDictionary counts;
    auto &measurements = job["measurementCounts"];
    
    cudaq::info("Found measurements: {}", measurements.dump());

    for (const auto &[bitstring, count] : measurements.items()) {
        counts[bitstring] = count.get<std::size_t>();
    }

    std::vector<ExecutionResult> execResults;
    execResults.emplace_back(ExecutionResult{counts});

    return cudaq::sample_result(execResults);
}


private:

  RestHeaders getHeaders() override {
    if (backendConfig.find("api_key") == backendConfig.end()) {
      throw std::runtime_error("API key not found in config. Was initialize() called?");
    }

    RestHeaders headers;
    headers["api-key"] = backendConfig.at("api_key");
    headers["Content-Type"] = "application/json";
    headers["User-Agent"] = backendConfig.at("user_agent");
    return headers;
  }

  std::string getEnvVar(const std::string &key, const std::string &defaultVal,
                        const bool isRequired) const {
    const char *env_var = std::getenv(key.c_str());
    if (env_var == nullptr) {
      if (isRequired)
        throw std::runtime_error(key + " environment variable is not set.");
      return defaultVal;
    }
    return std::string(env_var);
  }

  std::string getValueOrDefault(const BackendConfig &config,
                               const std::string &key,
                               const std::string &defaultValue) const {
    return config.find(key) != config.end() ? config.at(key) : defaultValue;
  }
};

}

CUDAQ_REGISTER_TYPE(cudaq::ServerHelper, cudaq::QbraidServerHelper, qbraid)