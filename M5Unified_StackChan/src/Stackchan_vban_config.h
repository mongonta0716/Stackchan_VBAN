#ifndef __STACKCHAN_VBAN_CONFIG_H__
#define __STACKCHAN_VBAN_CONFIG_H__

#include <Stackchan_system_config.h>

typedef struct StaticIpConfig {
  bool enabled;
  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns1;
  IPAddress dns2;
} static_ip_config_s;

class StackchanVbanConfig : public StackchanSystemConfig
{
  protected:
    static_ip_config_s _static_ip_config;

  public:
    StackchanVbanConfig();
    ~StackchanVbanConfig();

    void loadExtendConfig(fs::FS& fs, const char* yaml_filename, uint32_t yaml_size) override;
    void setExtendSettings(DynamicJsonDocument doc) override;
    void printExtParameters(void) override;
    const static_ip_config_s* getStaticIpConfig() const { return &_static_ip_config; }
};

#endif
