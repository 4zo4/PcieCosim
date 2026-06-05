/**
 * @file log.h
 * @brief Logging declarations for PCIe co-simulation.
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#pragma once

#include <string>
#include <cstdint>

#ifndef ENABLE_SW_LOGS
#define ENABLE_SW_LOGS 0
#endif
#define ENABLE_LOGS ENABLE_SW_LOGS
#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_CRITICAL 10
#define LOG_LEVEL_ERROR 20
#define LOG_LEVEL_WARNING 30
#define LOG_LEVEL_INFO 40
#define LOG_LEVEL_DEBUG 50
#if ENABLE_LOGS
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_ERROR
#endif
#if ENABLE_PKT_LOGS
#define LOG_PKT2(dir, p) log_pkt(dir, p, __func__)
#define LOG_PKT3(dir, p, dbuf) log_pkt(dir, p, __func__, dbuf)
#define LOG_PKT4(dir, p, name, dbuf) log_pkt(dir, p, name, dbuf)
#define LOG_PKT_SELECT(_1, _2, _3, _4, NAME, ...) NAME
#define LOG_PKT(...) LOG_PKT_SELECT(__VA_ARGS__, LOG_PKT4, LOG_PKT3, LOG_PKT2)(__VA_ARGS__)
#define LOG_CRITICAL(prefix, ...) log_msg(LOG_LEVEL_CRITICAL, prefix, __func__, __LINE__, __VA_ARGS__)
#else // !ENABLE_PKT_LOGS
#define LOG_PKT(...) NOP()
#endif
#define LOG_ERROR(prefix, ...) log_msg(LOG_LEVEL_ERROR, prefix, __func__, __LINE__, __VA_ARGS__)
#define LOG_WARNING(prefix, ...) log_msg(LOG_LEVEL_WARNING, prefix, __func__, __LINE__, __VA_ARGS__)
#define LOG_INFO(prefix, ...) log_msg(LOG_LEVEL_INFO, prefix, __func__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(prefix, ...) log_msg(LOG_LEVEL_DEBUG, prefix, __func__, __LINE__, __VA_ARGS__)
#else // !ENABLE_LOGS
#ifdef LOG_LEVEL
#undef LOG_LEVEL
#endif
#define LOG_LEVEL LOG_LEVEL_NONE
#define LOG_PKT(...) NOP()
#define LOG_CRITICAL(prefix, ...) NOP()
#define LOG_ERROR(prefix, ...) NOP()
#define LOG_WARNING(prefix, ...) NOP()
#define LOG_INFO(prefix, ...) NOP()
#define LOG_DEBUG(prefix, ...) NOP()
#endif

enum class Dir : uint8_t { RX = 1, TX = 2 };
void log_pkt(Dir direction, const Protocol& pkt, const std::string& tag, const char *dbuf = nullptr);
void log_msg(int level, const char *prefix, const char *func, const int line, const char* format, ...);

