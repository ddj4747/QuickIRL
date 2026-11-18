#pragma once

#include <QString>
#include <cstdint>

struct RelayConfig {
	uint16_t apiPort{8080};
	uint16_t obsControlPort{8081};
	uint16_t srtlaPort{5000};
	uint16_t srtPort{8890};
	uint16_t rtspPort{8554};
	uint16_t rtpPort{8000};
	uint16_t rtcpPort{8001};
	uint16_t rtmpPort{1935};
	uint16_t webRtcPort{8889};
	uint16_t moqPort{8892};
	uint16_t moqQuicPort{8893};
	uint16_t mediaApiPort{9997};

	static bool load(const QString &directory, RelayConfig &config, QString &error);
};
