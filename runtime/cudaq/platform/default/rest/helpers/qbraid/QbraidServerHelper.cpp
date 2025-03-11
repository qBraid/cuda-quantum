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
  static constexpr const char *DEFAULT_DEVICE = "ionq_simulator";
  static constexpr int DEFAULT_QUBITS = 29;  

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
    // result endpoints
    backendConfig["results_path"] = backendConfig["url"] + "/quantum-jobs/result/";

    // Add results output directory and file naming pattern
    backendConfig["results_output_dir"] = getValueOrDefault(config, "results_output_dir", "./qbraid_results");
    backendConfig["results_file_prefix"] = getValueOrDefault(config, "results_file_prefix", "qbraid_job_");

    if (!config["shots"].empty()) {
      backendConfig["shots"] = config["shots"];
      this->setShots(std::stoul(config["shots"]));
    } else {
      backendConfig["shots"] = "1000"; 
      this->setShots(1000);
    }

    parseConfigForCommonParams(config);

    cudaq::info("Qbraid configuration initialized:");
    for (const auto& [key, value] : backendConfig) {
      cudaq::info("  {} = {}", key, value);
    }
    
    // Create results directory if it doesn't exist
    std::string resultsDir = backendConfig["results_output_dir"];
    std::filesystem::create_directories(resultsDir);
    cudaq::info("Created results directory: {}", resultsDir);
  }

  ServerJobPayload createJob(std::vector<KernelExecution> &circuitCodes) override {
    if (backendConfig.find("job_path") == backendConfig.end()) {
      throw std::runtime_error("job_path not found in config. Was initialize() called?");
    }

    std::vector<ServerMessage> jobs;
    for (auto &circuitCode : circuitCodes) {
      ServerMessage job;
      job["qbraidDeviceId"] = backendConfig.at("device_id");
      job["openQasm"] = circuitCode.code;
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
    // Use qbraidJobId instead of vendorJobId
    return backendConfig.at("job_path") + "?qbraidJobId=" + 
           postResponse.at("qbraidJobId").get<std::string>();
  }

  std::string constructGetJobPath(std::string &jobId) override {
    // Use qbraidJobId instead of vendorJobId
    return backendConfig.at("job_path") + "?qbraidJobId=" + jobId;
  }

  // Getting results path - simplified to match working implementation
  std::string constructGetResultsPath(const std::string &jobId) {
    return backendConfig.at("results_path") + jobId;
  }

  // Job is done with sample results api - simplified based on working implementation
  bool jobIsDone(ServerMessage &getJobResponse) override {
    // Save the current job response to a file regardless of status
    saveResponseToFile(getJobResponse);
    
    std::string status;
    
    // Check if response has the jobsArray format 
    if (getJobResponse.contains("jobsArray") && !getJobResponse["jobsArray"].empty()) {
      status = getJobResponse["jobsArray"][0]["status"].get<std::string>();
      cudaq::info("Job status from jobs endpoint: {}", status);
    }
    // Check if it uses the direct format
    else if (getJobResponse.contains("status")) {
      status = getJobResponse["status"].get<std::string>();
      cudaq::info("Job status from direct response: {}", status);
    }
    // Or if it's in the data object
    else if (getJobResponse.contains("data") && getJobResponse["data"].contains("status")) {
      status = getJobResponse["data"]["status"].get<std::string>();
      cudaq::info("Job status from data object: {}", status);
    }
    else {
      cudaq::info("Unexpected job response format: {}", getJobResponse.dump());
      throw std::runtime_error("Invalid job response format");
    }
    
    if (status == "FAILED")
      throw std::runtime_error("The job failed upon submission. Check your qBraid account for more information.");
    
    return status == "COMPLETED";
  }

  // Sample results with results api - with retry logic
  cudaq::sample_result processResults(ServerMessage &getJobResponse,
                                   std::string &jobId) override {
    // Save the final job response to file
    saveResponseToFile(getJobResponse, jobId + "_final");
    
    // Try to get results using the direct results endpoint with retries
    int maxRetries = 5;
    int waitTime = 2;
    float backoffFactor = 2.0;
    
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
      try {
        auto resultsPath = constructGetResultsPath(jobId);
        auto headers = getHeaders();
        
        cudaq::info("Fetching results using direct endpoint (attempt {}/{}): {}", 
                   attempt + 1, maxRetries, resultsPath);
        RestClient client;
        auto resultJson = client.get("", resultsPath, headers, true);
        
        // Save direct results response to file
        saveResponseToFile(resultJson, jobId + "_direct_results_" + std::to_string(attempt));

        if (resultJson.contains("error") && !resultJson["error"].is_null()) {
          std::string errorMsg = resultJson["error"].is_string() ? 
                                resultJson["error"].get<std::string>() : 
                                resultJson["error"].dump();
          cudaq::info("Error from results endpoint: {}", errorMsg);
          
          // Only throw if on last attempt
          if (attempt == maxRetries - 1) {
            throw std::runtime_error("Error retrieving results: " + errorMsg);
          }
        }
        else if (resultJson.contains("data") && 
                resultJson["data"].contains("measurementCounts")) {
          cudaq::info("Processing results from direct endpoint");
          CountsDictionary counts;
          auto &measurements = resultJson["data"]["measurementCounts"];
          
          for (const auto &[bitstring, count] : measurements.items()) {
            counts[bitstring] = count.is_number() ? 
                               static_cast<std::size_t>(count.get<double>()) : 
                               static_cast<std::size_t>(count);
          }
          
          std::vector<ExecutionResult> execResults;
          execResults.emplace_back(ExecutionResult{counts});
          return cudaq::sample_result(execResults);
        }
        
        // If we get here, no valid data was found but also no error - retry
        if (attempt < maxRetries - 1) {
          int sleepTime = waitTime * std::pow(backoffFactor, attempt);
          cudaq::info("No valid results yet, retrying in {} seconds", sleepTime);
          std::this_thread::sleep_for(std::chrono::seconds(sleepTime));
        }
        
      } catch (const std::exception &e) {
        cudaq::info("Exception when using direct results endpoint: {}", e.what());
        if (attempt < maxRetries - 1) {
          int sleepTime = waitTime * std::pow(backoffFactor, attempt);
          cudaq::info("Retrying in {} seconds", sleepTime);
          std::this_thread::sleep_for(std::chrono::seconds(sleepTime));
        }
        else {
          cudaq::info("Falling back to original results processing method");
        }
      }
    }
    
    // Original result processing as fallback
    cudaq::info("Processing results from job response for job {}", jobId);
    if (getJobResponse.contains("jobsArray") && !getJobResponse["jobsArray"].empty()) {
      auto &job = getJobResponse["jobsArray"][0];
      
      if (job.contains("measurementCounts")) {
        CountsDictionary counts;
        auto &measurements = job["measurementCounts"];
        
        for (const auto &[bitstring, count] : measurements.items()) {
          counts[bitstring] = count.get<std::size_t>();
        }

        std::vector<ExecutionResult> execResults;
        execResults.emplace_back(ExecutionResult{counts});
        return cudaq::sample_result(execResults);
      }
    }
    
    // Last resort - check for direct measurementCounts in the response
    if (getJobResponse.contains("measurementCounts")) {
      CountsDictionary counts;
      auto &measurements = getJobResponse["measurementCounts"];
      
      for (const auto &[bitstring, count] : measurements.items()) {
        counts[bitstring] = count.get<std::size_t>();
      }

      std::vector<ExecutionResult> execResults;
      execResults.emplace_back(ExecutionResult{counts});
      return cudaq::sample_result(execResults);
    }
    
    throw std::runtime_error("No measurement counts found in any response format");
  }

private:
  // New method to save response to file
  void saveResponseToFile(const ServerMessage &response, const std::string &identifier = "") {
    try {
      std::string outputDir = backendConfig.at("results_output_dir");
      std::string filePrefix = backendConfig.at("results_file_prefix");
      
      // Create a unique filename using timestamp if no identifier provided
      std::string filename;
      if (identifier.empty()) {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        filename = outputDir + "/" + filePrefix + std::to_string(timestamp) + ".json";
      } else {
        filename = outputDir + "/" + filePrefix + identifier + ".json";
      }
      
      // Write the JSON response to the file with proper formatting
      std::ofstream outputFile(filename);
      if (!outputFile.is_open()) {
        cudaq::info("Failed to open file for writing: {}", filename);
        return;
      }
      
      outputFile << response.dump(2); // 2 spaces for indentation
      outputFile.close();
      
      cudaq::info("Response saved to file: {}", filename);
    } catch (const std::exception &e) {
      cudaq::info("Error saving response to file: {}", e.what());
    }
  }

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
