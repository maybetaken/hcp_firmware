// Host tests for hcp::provision's Fsm: the connect/credential/BLE policy.
#include "hcp/provision/fsm.h"

#include <gtest/gtest.h>

using hcp::provision::Fsm;

namespace {

bool Has(const Fsm::Output& o, Fsm::Action a) {
  for (int i = 0; i < o.num_actions; i++) {
    if (o.actions[i] == a) {
      return true;
    }
  }
  return false;
}

int Count(const Fsm::Output& o, Fsm::Action a) {
  int n = 0;
  for (int i = 0; i < o.num_actions; i++) {
    if (o.actions[i] == a) {
      n++;
    }
  }
  return n;
}

}  // namespace

// ---- boot ----

TEST(ProvisionFsm, BootWithoutCredsOpensBle) {
  Fsm fsm(3);
  Fsm::Output o = fsm.Start(false);
  EXPECT_EQ(o.state, Fsm::State::kProvisioning);
  EXPECT_EQ(o.pattern, Fsm::Pattern::kBlueSlow);
  EXPECT_TRUE(Has(o, Fsm::Action::kEnableBle));
  EXPECT_FALSE(Has(o, Fsm::Action::kConnectWifi));
  EXPECT_EQ(o.alert, Fsm::Alert::kNone);
}

TEST(ProvisionFsm, BootWithCredsTriesWifiAndKeepsBleClosed) {
  Fsm fsm(3);
  Fsm::Output o = fsm.Start(true);
  EXPECT_EQ(o.state, Fsm::State::kWifiTry);
  EXPECT_EQ(o.pattern, Fsm::Pattern::kBlueFast);
  EXPECT_TRUE(Has(o, Fsm::Action::kConnectWifi));
  EXPECT_FALSE(Has(o, Fsm::Action::kEnableBle));
  EXPECT_TRUE(fsm.ble_open() == false);
}

// ---- saved-credentials retry budget ----

TEST(ProvisionFsm, SavedCredsRetryThenFallbackToBle) {
  Fsm fsm(3);  // three tries in total
  fsm.Start(true);
  EXPECT_EQ(fsm.attempts(), 1);

  Fsm::Output first = fsm.OnWifiConnectFailed();
  EXPECT_EQ(first.state, Fsm::State::kWifiTry);
  EXPECT_TRUE(Has(first, Fsm::Action::kConnectWifi));
  EXPECT_FALSE(Has(first, Fsm::Action::kEnableBle));
  EXPECT_EQ(fsm.attempts(), 2);

  Fsm::Output second = fsm.OnWifiConnectFailed();
  EXPECT_EQ(second.state, Fsm::State::kWifiTry);
  EXPECT_TRUE(Has(second, Fsm::Action::kConnectWifi));
  EXPECT_EQ(fsm.attempts(), 3);

  Fsm::Output third = fsm.OnWifiConnectFailed();
  EXPECT_EQ(third.state, Fsm::State::kProvisioning);
  EXPECT_TRUE(Has(third, Fsm::Action::kEnableBle));
  EXPECT_EQ(third.alert, Fsm::Alert::kRed3);
  EXPECT_EQ(third.pattern, Fsm::Pattern::kBlueSlow);

  // No further retries once we are provisioning.
  Fsm::Output extra = fsm.OnWifiConnectFailed();
  EXPECT_EQ(extra.state, Fsm::State::kProvisioning);
  EXPECT_EQ(extra.num_actions, 0);
}

TEST(ProvisionFsm, AttemptsAreCountedAndResetOnFallback) {
  Fsm fsm(2);
  fsm.Start(true);
  EXPECT_EQ(fsm.attempts(), 1);

  fsm.OnWifiConnectFailed();
  EXPECT_EQ(fsm.attempts(), 2);
  EXPECT_EQ(fsm.state(), Fsm::State::kWifiTry);

  fsm.OnWifiConnectFailed();  // second failure exhausts the budget
  EXPECT_EQ(fsm.state(), Fsm::State::kProvisioning);
  EXPECT_EQ(fsm.attempts(), 0);  // fresh budget the next time we try Wi-Fi
}

// ---- phone-driven provisioning ----

TEST(ProvisionFsm, PhoneCredsAreSavedAndBleStaysOpen) {
  Fsm fsm(3);
  fsm.Start(false);  // BLE already open

  Fsm::Output o = fsm.OnPhoneCreds();
  EXPECT_EQ(o.state, Fsm::State::kWifiTry);
  EXPECT_EQ(o.pattern, Fsm::Pattern::kBlueFast);
  EXPECT_TRUE(Has(o, Fsm::Action::kSaveCreds));
  // The BluFi layer issues the connect itself - never double-connect.
  EXPECT_FALSE(Has(o, Fsm::Action::kConnectWifi));
  // And it must stay reachable until the result is delivered.
  EXPECT_FALSE(Has(o, Fsm::Action::kDisableBle));
}

TEST(ProvisionFsm, PhoneProvisioningSuccessClosesBleDelayed) {
  Fsm fsm(3);
  fsm.Start(false);
  fsm.OnPhoneCreds();

  Fsm::Output o = fsm.OnWifiConnected();
  EXPECT_EQ(o.state, Fsm::State::kOnline);
  EXPECT_EQ(o.pattern, Fsm::Pattern::kGreenSolid);
  EXPECT_TRUE(Has(o, Fsm::Action::kCloseBleDelayed));
  EXPECT_FALSE(Has(o, Fsm::Action::kDisableBle));  // exactly one close request
  EXPECT_EQ(Count(o, Fsm::Action::kCloseBleDelayed), 1);
  EXPECT_FALSE(fsm.ble_open());
}

TEST(ProvisionFsm, BootSuccessDoesNotTouchBle) {
  Fsm fsm(3);
  fsm.Start(true);
  Fsm::Output o = fsm.OnWifiConnected();
  EXPECT_EQ(o.state, Fsm::State::kOnline);
  EXPECT_EQ(o.num_actions, 0);
}

TEST(ProvisionFsm, PhoneProvisioningFailureKeepsBleOpen) {
  Fsm fsm(3);
  fsm.Start(false);
  fsm.OnPhoneCreds();

  Fsm::Output o = fsm.OnWifiConnectFailed();
  EXPECT_EQ(o.state, Fsm::State::kProvisioning);
  EXPECT_EQ(o.alert, Fsm::Alert::kRed3);
  // BLE is already open; re-opening would tear the link down for nothing.
  EXPECT_FALSE(Has(o, Fsm::Action::kEnableBle));
  EXPECT_TRUE(fsm.ble_open());
}

// ---- link lost while online ----

TEST(ProvisionFsm, LinkLostWhileOnlineReconnects) {
  Fsm fsm(3);
  fsm.Start(true);
  fsm.OnWifiConnected();

  Fsm::Output o = fsm.OnWifiDisconnected();
  EXPECT_EQ(o.state, Fsm::State::kWifiTry);
  EXPECT_TRUE(Has(o, Fsm::Action::kConnectWifi));
  EXPECT_EQ(o.pattern, Fsm::Pattern::kBlueFast);
}

TEST(ProvisionFsm, LinkLostIgnoredWhenNotOnline) {
  Fsm fsm(3);
  fsm.Start(false);
  Fsm::Output o = fsm.OnWifiDisconnected();
  EXPECT_EQ(o.state, Fsm::State::kProvisioning);
  EXPECT_EQ(o.num_actions, 0);
}

TEST(ProvisionFsm, DisconnectWhileTryingIsNotAFailedAttempt) {
  // Our own pre-connect Disconnect() reports a disconnect; consuming the retry
  // budget for it would fall back to BluFi prematurely.
  Fsm fsm(3);
  fsm.Start(true);
  Fsm::Output o = fsm.OnWifiDisconnected();
  EXPECT_EQ(o.state, Fsm::State::kWifiTry);
  EXPECT_EQ(o.num_actions, 0);
  EXPECT_EQ(fsm.attempts(), 1);  // budget untouched
}

TEST(ProvisionFsm, LostThenAllRetriesFailOpensBleAgain) {
  Fsm fsm(2);
  fsm.Start(true);
  fsm.OnWifiConnected();
  fsm.OnWifiDisconnected();       // attempt 1 of the reconnect cycle
  fsm.OnWifiConnectFailed();      // -> attempt 2
  Fsm::Output o = fsm.OnWifiConnectFailed();
  EXPECT_EQ(o.state, Fsm::State::kProvisioning);
  EXPECT_TRUE(Has(o, Fsm::Action::kEnableBle));
}

// ---- per-attempt deadline (the ESP32 driver is silent on failed association) ----

TEST(ProvisionFsm, AttemptTimeoutRetriesThenFallsBackToBle) {
  Fsm fsm(2);
  fsm.Start(true);

  Fsm::Output first = fsm.OnAttemptTimeout();
  EXPECT_EQ(first.state, Fsm::State::kWifiTry);
  EXPECT_TRUE(Has(first, Fsm::Action::kConnectWifi));

  Fsm::Output second = fsm.OnAttemptTimeout();
  EXPECT_EQ(second.state, Fsm::State::kProvisioning);
  EXPECT_TRUE(Has(second, Fsm::Action::kEnableBle));
  EXPECT_EQ(second.alert, Fsm::Alert::kRed3);
}

TEST(ProvisionFsm, AttemptTimeoutIgnoredWhenNotTrying) {
  Fsm fsm(3);
  fsm.Start(false);  // provisioning: no attempt in flight
  Fsm::Output o = fsm.OnAttemptTimeout();
  EXPECT_EQ(o.state, Fsm::State::kProvisioning);
  EXPECT_EQ(o.num_actions, 0);
}

TEST(ProvisionFsm, LateSuccessAfterPhoneFailureStillClosesBle) {
  // Slow DHCP: the attempt times out (red3) but the connection lands later - the
  // phone-provisioning intent must survive so BLE still gets closed.
  Fsm fsm(3);
  fsm.Start(false);
  fsm.OnPhoneCreds();
  fsm.OnAttemptTimeout();
  Fsm::Output o = fsm.OnWifiConnected();
  EXPECT_EQ(o.state, Fsm::State::kOnline);
  EXPECT_TRUE(Has(o, Fsm::Action::kCloseBleDelayed));
}

// ---- button: long press resets configuration ----

TEST(ProvisionFsm, ResetErasesCredsAndOpensBle) {
  Fsm fsm(3);
  fsm.Start(true);  // BLE closed, Wi-Fi being tried
  Fsm::Output o = fsm.OnResetRequested();
  EXPECT_EQ(o.state, Fsm::State::kProvisioning);
  EXPECT_TRUE(Has(o, Fsm::Action::kEraseCreds));
  EXPECT_TRUE(Has(o, Fsm::Action::kEnableBle));
  EXPECT_EQ(o.alert, Fsm::Alert::kPurple3);
}

TEST(ProvisionFsm, ResetWhileProvisioningDoesNotReopenBle) {
  Fsm fsm(3);
  fsm.Start(false);  // BLE open
  Fsm::Output o = fsm.OnResetRequested();
  EXPECT_EQ(o.state, Fsm::State::kProvisioning);
  EXPECT_TRUE(Has(o, Fsm::Action::kEraseCreds));
  EXPECT_FALSE(Has(o, Fsm::Action::kEnableBle));
}

TEST(ProvisionFsm, ResetClearsThePhonePendingFlag) {
  Fsm fsm(3);
  fsm.Start(false);
  fsm.OnPhoneCreds();
  fsm.OnResetRequested();
  // After a reset a later success must not look like phone provisioning.
  Fsm::Output o = fsm.OnWifiConnected();
  EXPECT_EQ(o.state, Fsm::State::kOnline);
  EXPECT_FALSE(Has(o, Fsm::Action::kCloseBleDelayed));
}
