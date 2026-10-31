#pragma once
#include <HTP/QnnHtpDevice.h>

// One inference owns one vote. Keeping the model loaded must not keep the DSP awake.
class QnnPowerVote {
 public:
  explicit QnnPowerVote(const QnnHtpDevice_PerfInfrastructure_t &infra) : infra_(infra) {}
  ~QnnPowerVote() { release(); }
  QnnPowerVote(const QnnPowerVote &) = delete;
  QnnPowerVote &operator=(const QnnPowerVote &) = delete;

  bool apply(int thermal, bool legacy = false) {
    if (thermal >= 4) { release(); return false; }
    if (failed_) return false;
    if (active_ && level_ == thermal && legacy_ == legacy) return true;
    if (!infra_.createPowerConfigId || !infra_.setPowerConfig || !infra_.destroyPowerConfigId) {
      failed_ = true;
      return false;
    }
    if (!active_) {
      const auto status = infra_.createPowerConfigId(0, 0, &id_);
      if (status != QNN_SUCCESS || id_ == 0) { failed_ = true; return false; }
      active_ = true;
    }
    auto dcvs = QnnHtpPerfInfrastructure_PowerConfig_t QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIG_INIT;
    dcvs.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
    auto &config = dcvs.dcvsV3Config;
    config.contextId = id_;
    config.setDcvsEnable = 1;
    config.dcvsEnable = 1;
    config.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_ADJUST_UP_DOWN;
    config.setSleepLatency = 1;
    config.sleepLatency = 100;
    config.setSleepDisable = 1;
    config.sleepDisable = 0;
    const auto target = thermal >= 3 ? DCVS_VOLTAGE_VCORNER_SVS_PLUS
        : thermal >= 2 ? DCVS_VOLTAGE_VCORNER_NOM
        : thermal == 1 || legacy ? DCVS_VOLTAGE_VCORNER_NOM_PLUS : DCVS_VOLTAGE_VCORNER_TURBO;
    const auto maximum = thermal <= 1 && !(legacy && thermal == 1) ? DCVS_VOLTAGE_VCORNER_TURBO : target;
    config.setBusParams = config.setCoreParams = 1;
    // A cold, active inference gets a burst floor. Scope destruction still removes
    // the entire request when work ends; warming immediately restores a DCVS range.
    config.busVoltageCornerMin = config.coreVoltageCornerMin =
        thermal == 0 && !legacy ? DCVS_VOLTAGE_VCORNER_TURBO : DCVS_VOLTAGE_VCORNER_SVS2;
    config.busVoltageCornerTarget = config.coreVoltageCornerTarget = target;
    config.busVoltageCornerMax = config.coreVoltageCornerMax = maximum;

    auto polling = QnnHtpPerfInfrastructure_PowerConfig_t QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIG_INIT;
    polling.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_RPC_POLLING_TIME;
    polling.rpcPollingTimeConfig = 0; // Stop process-wide CPU busy polling after every tile.
    const QnnHtpPerfInfrastructure_PowerConfig_t *configs[] = {&dcvs, &polling, nullptr};
    if (infra_.setPowerConfig(id_, configs) != QNN_SUCCESS) {
      failed_ = true;
      release();
      return false;
    }
    level_ = thermal;
    legacy_ = legacy;
    return true;
  }

  void release() {
    if (active_) infra_.destroyPowerConfigId(id_);
    active_ = false;
    level_ = -1;
    id_ = 0;
  }
 private:
  QnnHtpDevice_PerfInfrastructure_t infra_;
  uint32_t id_ = 0;
  int level_ = -1;
  bool active_ = false;
  bool failed_ = false;
  bool legacy_ = false;
};
