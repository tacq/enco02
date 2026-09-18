#pragma once

#include <string>
#include <memory>

struct Config {
  struct Mqtt {
    std::string endpoint;
    std::string client_id;
    std::string username;
    std::string password;
    std::string publish_topic;
    std::string subscribe_topic;
  };

  struct Activation {
    std::string code;
    std::string message;
  };

  Mqtt mqtt;
  Activation activation;
};

std::shared_ptr<Config> GetConfigFromServer(const std::string& url, const std::string& uuid);
