/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "taskheartbeat.h"
#include "leakdetector.h"
#include "logger.h"
#include "mozillavpn.h"
#include "networkrequest.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {
Logger logger(LOG_MAIN, "TaskHeartbeat");
}

TaskHeartbeat::TaskHeartbeat() : Task("TaskHeartbeat") {
  MVPN_COUNT_CTOR(TaskHeartbeat);
}

TaskHeartbeat::~TaskHeartbeat() { MVPN_COUNT_DTOR(TaskHeartbeat); }

void TaskHeartbeat::run(MozillaVPN* vpn) {
  NetworkRequest* request = NetworkRequest::createForHeartbeat(this);

  connect(request, &NetworkRequest::requestFailed,
          [this](QNetworkReply::NetworkError, const QByteArray&) {
            logger.log() << "Failed to talk with the server";
            // We don't know if this happeneded because of a global network
            // failure or a local network issue. In general, let's ignore this
            // error.
            emit completed();
          });

  connect(request, &NetworkRequest::requestCompleted,
          [this, vpn](const QByteArray& data) {
            logger.log() << "Heartbeat content received:" << data;

            QJsonObject json = QJsonDocument::fromJson(data).object();
            QJsonValue mullvad = json.value("mullvadOK");
            QJsonValue db = json.value("dbOK");
            if ((mullvad.isBool() && db.isBool()) &&
                (!mullvad.toBool() || !db.toBool())) {
              vpn->heartbeatFailure();
            }

            emit completed();
          });
}
