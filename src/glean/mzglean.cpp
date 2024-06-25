/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mzglean.h"

#include "constants.h"
#include "feature/feature.h"
#include "glean/generated/metrics.h"
#include "glean/generated/pings.h"
#include "leakdetector.h"
#include "logger.h"
#include "settingsholder.h"
#if not(defined(MZ_WASM))
#  include "qtglean.h"
#endif
#if defined(MZ_ANDROID)
#  include "../platforms/android/androidvpnactivity.h"
#  include "platforms/android/androidcommons.h"
#endif
#if defined(MZ_IOS)
#  include "platforms/ios/iosgleanbridge.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStandardPaths>
#if defined(MZ_ANDROID)
#  include <QJsonDocument>
#  include <QJsonObject>
#endif

namespace {
Logger logger("Glean");
MZGlean* s_instance = nullptr;

QString rootAppFolder() {
#if defined(UNIT_TEST)
  return QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#else
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
}
}  // namespace

MZGlean::MZGlean(QObject* parent) : QObject(parent) {
  MZ_COUNT_CTOR(MZGlean);

#if defined(MZ_ANDROID)
  connect(AndroidVPNActivity::instance(),
          &AndroidVPNActivity::eventRequestGleanUploadEnabledState, this,
          [&]() {
            broadcastUploadEnabledChange(
                SettingsHolder::instance()->gleanEnabled());
          });
#endif
}

MZGlean::~MZGlean() { MZ_COUNT_DTOR(MZGlean); }

// static
void MZGlean::registerLogHandler(void (*messageHandler)(int32_t, char*)) {
#if defined(MZ_WASM)
  Q_UNUSED(messageHandler);
#else
  glean_register_log_handler(messageHandler);
#endif
}

// static
void MZGlean::initialize(const QString& channel) {
  logger.debug() << "Initializing MZGlean";

  if (Feature::get(Feature::Feature_gleanRust)->isSupported()) {
    if (!s_instance) {
      s_instance = new MZGlean(qApp);

      connect(SettingsHolder::instance(), &SettingsHolder::gleanEnabledChanged,
              s_instance, []() {
                s_instance->setUploadEnabled(
                    SettingsHolder::instance()->gleanEnabled());
              });
    }

    QDir gleanDirectory(rootAppFolder());

    if (!gleanDirectory.exists(GLEAN_DATA_DIRECTORY) &&
        !gleanDirectory.mkpath(GLEAN_DATA_DIRECTORY)) {
      logger.error()
          << "Unable to create the MZGlean data directory. Terminating."
          << rootAppFolder();
      return;
    }

    if (!gleanDirectory.cd(GLEAN_DATA_DIRECTORY)) {
      logger.error()
          << "Unable to open the MZGlean data directory. Terminating.";
      return;
    }

#ifndef MZ_WASM
    if (channel == "testing") {
      glean_test_reset_glean(SettingsHolder::instance()->gleanEnabled(),
                             gleanDirectory.absolutePath().toUtf8(),
                             QLocale::system().name().toUtf8());
      return;
    }
#  if defined(MZ_IOS)
    new IOSGleanBridge(SettingsHolder::instance()->gleanEnabled(), channel);
#  elif defined(MZ_ANDROID)
    AndroidCommons::initializeGlean(SettingsHolder::instance()->gleanEnabled(),
                                    channel);
#  else
    SettingsHolder* settingsHolder = SettingsHolder::instance();
    Q_ASSERT(settingsHolder);

    glean_initialize(SettingsHolder::instance()->gleanEnabled(),
                     gleanDirectory.absolutePath().toUtf8(), channel.toUtf8(),
                     QLocale::system().name().toUtf8());

    setLogPings(settingsHolder->gleanLogPings());
    if (settingsHolder->gleanDebugTagActive()) {
      setDebugViewTag(settingsHolder->gleanDebugTag());
    }
#  endif
#endif
  }
}

// static
void MZGlean::setUploadEnabled(bool isTelemetryEnabled) {
  logger.debug() << "Changing MZGlean upload status to" << isTelemetryEnabled;

#if not(defined(MZ_WASM))
  glean_set_upload_enabled(isTelemetryEnabled);
#endif

  broadcastUploadEnabledChange(isTelemetryEnabled);

  if (isTelemetryEnabled) {
#if defined(MZ_ANDROID) || defined(MZ_IOS)
    // need to reset installation ID, as it would have been cleared
    QString uuid = mozilla::glean::session::installation_id.generateAndSet();
    SettingsHolder::instance()->setInstallationId(uuid);
#endif
  } else {
    // Note: Identifiers should be rotated when telemetry is disabled.
    //
    // Whenever telemetry is disabled, that generates a data deletion request
    // on the backend. If telemtry is re-enabled, all new identifiers are
    // generated by Glean, so we also rotate our identifiers to comply.

    // Clear out the experimentation ID
    SettingsHolder::instance()->removeUnauthedExperimenterId();
#if defined(MZ_ANDROID) || defined(MZ_IOS)
    // Clear out the former installation ID
    SettingsHolder::instance()->removeInstallationId();
#endif
  }
}

// static
void MZGlean::broadcastUploadEnabledChange(bool isTelemetryEnabled) {
#if defined(MZ_ANDROID)
  logger.debug() << "Broadcasting MZGlean upload status to Android Daemon.";

  QJsonObject args;
  args["uploadEnabled"] = isTelemetryEnabled;
  QJsonDocument doc(args);
  AndroidVPNActivity::instance()->sendToService(
      ServiceAction::ACTION_SET_GLEAN_UPLOAD_ENABLED,
      doc.toJson(QJsonDocument::Compact));
#endif

#if defined(MZ_IOS)
  logger.debug()
      << "Broadcasting MZGlean upload status to iOS NetworkExtension.";
  IOSGleanBridge::syncTelemetryEnabled(isTelemetryEnabled);
#endif
}

// static
void MZGlean::shutdown() {
#if not(defined(MZ_WASM))
  glean_shutdown();
#endif
}

// static
void MZGlean::setDebugViewTag(QString tag) {
  SettingsHolder* settingsHolder = SettingsHolder::instance();
  Q_ASSERT(settingsHolder);

  if (tag.isEmpty()) {
    logger.debug() << "Removing MZGlean debug view tag";
    settingsHolder->removeGleanDebugTag();
    settingsHolder->setGleanDebugTagIsActive(false);
    return;
  }

  logger.debug() << "Setting MZGlean debug view tag to" << tag;
  settingsHolder->setGleanDebugTag(tag);
  settingsHolder->setGleanDebugTagIsActive(true);

#if not(defined(MZ_WASM))
  glean_set_debug_view_tag(tag.toUtf8());
#endif
}

// static
void MZGlean::setLogPings(bool flag) {
  logger.debug() << "Setting MZGlean log pings value to" << flag;

  SettingsHolder* settingsHolder = SettingsHolder::instance();
  Q_ASSERT(settingsHolder);

  settingsHolder->setGleanLogPings(flag);
#if not(defined(MZ_WASM))
  glean_set_log_pings(flag);
#endif
}
