#pragma once

#include <RelayConfig.h>
#include <QString>
#include <cstdint>

namespace MediaMtxConfig {
bool write(const QString &path, const RelayConfig &config, QString &error);
} // namespace MediaMtxConfig
