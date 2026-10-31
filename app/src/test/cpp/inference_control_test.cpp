#include "inference_control.h"
#include "qnn_power.h"
#include <cassert>
#include <iostream>
#include <thread>

static int created, applied, destroyed;
static bool fail_create, fail_apply;
static QnnHtpPerfInfrastructure_DcvsV3_t last_config{};
static Qnn_ErrorHandle_t create_id(uint32_t, uint32_t, uint32_t *id) {
  ++created; *id = 17; return fail_create ? 1 : QNN_SUCCESS;
}
static Qnn_ErrorHandle_t destroy_id(uint32_t id) { assert(id == 17); ++destroyed; return QNN_SUCCESS; }
static Qnn_ErrorHandle_t set_config(uint32_t id, const QnnHtpPerfInfrastructure_PowerConfig_t **configs) {
  assert(id == 17); ++applied;
  assert(configs[0]->option == QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3);
  last_config = configs[0]->dcvsV3Config;
  assert(configs[1]->option == QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_RPC_POLLING_TIME);
  assert(configs[1]->rpcPollingTimeConfig == 0 && configs[2] == nullptr);
  return fail_apply ? 1 : QNN_SUCCESS;
}
static void reset() { created = applied = destroyed = 0; fail_create = fail_apply = false; }
static void power_lifecycle() {
  QnnHtpDevice_PerfInfrastructure_t infra = QNN_HTP_DEVICE_PERF_INFRASTRUCTURE_INIT;
  infra.createPowerConfigId = create_id;
  infra.destroyPowerConfigId = destroy_id;
  infra.setPowerConfig = set_config;
  reset();
  {
    QnnPowerVote vote(infra);
    assert(created == 0); // Loading a model alone casts no vote.
    assert(vote.apply(0));
    assert(created == 1 && applied == 1);
    assert(last_config.dcvsEnable == 1 && last_config.sleepDisable == 0);
    assert(last_config.coreVoltageCornerMin == DCVS_VOLTAGE_VCORNER_TURBO);
    assert(last_config.coreVoltageCornerTarget == DCVS_VOLTAGE_VCORNER_TURBO);
    assert(last_config.coreVoltageCornerMax < DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER);
    assert(vote.apply(0) && applied == 1); // No per-tile RPC overhead at stable temperature.
    auto previous_max = last_config.coreVoltageCornerMax;
    for (int level = 1; level <= 3; ++level) {
      assert(vote.apply(level));
      assert(last_config.coreVoltageCornerMax <= previous_max);
      assert(last_config.coreVoltageCornerMin < last_config.coreVoltageCornerMax);
      if (level == 1) {
        assert(last_config.coreVoltageCornerTarget == DCVS_VOLTAGE_VCORNER_NOM_PLUS);
        assert(last_config.coreVoltageCornerMax == DCVS_VOLTAGE_VCORNER_TURBO);
      }
      previous_max = last_config.coreVoltageCornerMax;
    }
    assert(!vote.apply(4) && destroyed == 1);
    assert(vote.apply(0) && created == 2); // Resume with a new, owned vote after cooling.
    assert(vote.apply(0, true)); // Same-build ADB baseline uses the 1.3.2 policy.
    assert(last_config.coreVoltageCornerMin == DCVS_VOLTAGE_VCORNER_SVS2);
    assert(last_config.coreVoltageCornerTarget == DCVS_VOLTAGE_VCORNER_NOM_PLUS);
    assert(last_config.coreVoltageCornerMax == DCVS_VOLTAGE_VCORNER_TURBO);
    const int before_repeat = applied;
    assert(vote.apply(0, true) && applied == before_repeat);
    assert(vote.apply(1, true));
    assert(last_config.coreVoltageCornerMax == DCVS_VOLTAGE_VCORNER_NOM_PLUS);
    assert(vote.apply(0));
    assert(last_config.coreVoltageCornerMin == DCVS_VOLTAGE_VCORNER_TURBO);
  }
  assert(destroyed == 2);
  reset(); fail_apply = true;
  {
    QnnPowerVote vote(infra);
    assert(!vote.apply(0) && destroyed == 1);
    assert(!vote.apply(0) && applied == 1); // A rejected hint must not be retried per tile.
  }
  assert(destroyed == 1);
  reset(); fail_create = true;
  { QnnPowerVote vote(infra); assert(!vote.apply(0)); }
  assert(created == 1 && applied == 0 && destroyed == 0);
  reset();
  auto failed_inference = [&] {
    QnnPowerVote vote(infra);
    assert(vote.apply(0));
    return -1; // An early inference failure also releases the vote.
  };
  assert(failed_inference() == -1 && destroyed == 1);
}
static void cancellation_ownership() {
  InferenceCancellation control;
  std::atomic<bool> abort{false};
  {
    InferenceCancellation::Scope first(control, 41, abort);
    control.cancel(40); assert(!abort.load());
    control.cancel(41); assert(abort.load());
  }
  abort.store(false);
  control.cancel(41); assert(!abort.load());
  {
    InferenceCancellation::Scope next(control, 42, abort);
    std::thread delayed([&] { for (int i = 0; i < 10000; ++i) control.cancel(41); });
    delayed.join();
    assert(!abort.load());
    control.cancel(42); assert(abort.load());
  }
  abort.store(false); control.cancel(42); assert(!abort.load());
}
int main() {
  power_lifecycle(); cancellation_ownership();
  std::cout << "Scoped QNN power, temperature policy and cancellation ownership checks passed.\n";
}
