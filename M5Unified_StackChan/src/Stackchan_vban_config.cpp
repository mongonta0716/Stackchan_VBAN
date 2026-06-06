#include "Stackchan_vban_config.h"

StackchanVbanConfig::StackchanVbanConfig()
{
  _static_ip_config = {};
}

StackchanVbanConfig::~StackchanVbanConfig() {}

static bool parseIpAddress(const char* value, IPAddress& address)
{
  return value != nullptr && value[0] != '\0' && address.fromString(value);
}

static bool parseIpAddress(const String& value, IPAddress& address)
{
  return value.length() > 0 && address.fromString(value);
}

static IPAddress defaultGatewayForIp(const IPAddress& ip)
{
  return IPAddress(ip[0], ip[1], ip[2], 1);
}

void StackchanVbanConfig::loadExtendConfig(fs::FS& fs, const char* yaml_filename, uint32_t yaml_size)
{
  M5_LOGI("----- StackchanVbanConfig::loadConfig:%s", yaml_filename);
  File file = fs.open(yaml_filename);
  if (file) {
    DynamicJsonDocument doc(yaml_size);
    auto err = deserializeYml(doc, file);
    if (err) {
      M5_LOGE("yaml file read error: %s", yaml_filename);
      M5_LOGE("error%s", err.c_str());
      return;
    }
    serializeJsonPretty(doc, Serial);
    setExtendSettings(doc);
  }
}

void StackchanVbanConfig::setExtendSettings(DynamicJsonDocument doc)
{
  _static_ip_config = {};

  JsonVariant static_ip = doc["STATIC_IP"];
  if (static_ip.isNull()) {
    static_ip = doc["wifi"]["STATIC_IP"];
  }
  if (static_ip.isNull()) return;

  String ip_value;
  if (static_ip.is<const char*>()) {
    ip_value = static_ip.as<String>();
  } else {
    ip_value = static_ip["ip"].as<String>();
    if (ip_value.length() == 0) ip_value = static_ip["address"].as<String>();
    if (ip_value.length() == 0) ip_value = static_ip["local_ip"].as<String>();
  }

  IPAddress ip;
  if (!parseIpAddress(ip_value, ip)) {
    if (ip_value.length() > 0) {
      M5_LOGE("Invalid STATIC_IP ip: %s", ip_value.c_str());
    }
    return;
  }

  IPAddress gateway = defaultGatewayForIp(ip);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns1 = gateway;
  IPAddress dns2(0, 0, 0, 0);
  if (static_ip.is<JsonObject>()) {
    parseIpAddress(static_ip["gateway"].as<const char*>(), gateway);
    parseIpAddress(static_ip["subnet"].as<const char*>(), subnet);
    parseIpAddress(static_ip["dns1"].as<const char*>(), dns1);
    parseIpAddress(static_ip["dns2"].as<const char*>(), dns2);
  }

  _static_ip_config.enabled = true;
  _static_ip_config.ip = ip;
  _static_ip_config.gateway = gateway;
  _static_ip_config.subnet = subnet;
  _static_ip_config.dns1 = dns1;
  _static_ip_config.dns2 = dns2;
}

void StackchanVbanConfig::printExtParameters(void)
{
  if (!_static_ip_config.enabled) {
    M5_LOGI("STATIC_IP: disabled");
    return;
  }
  M5_LOGI("STATIC_IP enabled ip:%s gateway:%s subnet:%s dns1:%s dns2:%s",
          _static_ip_config.ip.toString().c_str(),
          _static_ip_config.gateway.toString().c_str(),
          _static_ip_config.subnet.toString().c_str(),
          _static_ip_config.dns1.toString().c_str(),
          _static_ip_config.dns2.toString().c_str());
}
