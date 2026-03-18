// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_JSON_H_
#define WEF_JSON_H_

#include "webview_value.h"

#include <sstream>
#include <cstring>

namespace json {

inline std::string Escape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

inline std::string Serialize(const wef::ValuePtr& value);

inline std::string SerializeList(const wef::ValueList& list) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i > 0) ss << ",";
    ss << Serialize(list[i]);
  }
  ss << "]";
  return ss.str();
}

inline std::string SerializeDict(const wef::ValueDict& dict) {
  std::ostringstream ss;
  ss << "{";
  bool first = true;
  for (const auto& pair : dict) {
    if (!first) ss << ",";
    first = false;
    ss << "\"" << Escape(pair.first) << "\":" << Serialize(pair.second);
  }
  ss << "}";
  return ss.str();
}

inline std::string Serialize(const wef::ValuePtr& value) {
  if (!value) return "null";
  switch (value->type) {
    case wef::ValueType::Null:
      return "null";
    case wef::ValueType::Bool:
      return value->GetBool() ? "true" : "false";
    case wef::ValueType::Int:
      return std::to_string(value->GetInt());
    case wef::ValueType::Double: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.17g", value->GetDouble());
      return buf;
    }
    case wef::ValueType::String:
      return "\"" + Escape(value->GetString()) + "\"";
    case wef::ValueType::Binary: {
      const auto& binary = value->GetBinary();
      std::string base64;
      static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      size_t i = 0;
      const uint8_t* data = binary.data.data();
      size_t len = binary.data.size();
      while (i < len) {
        uint32_t n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        base64 += chars[(n >> 18) & 0x3F];
        base64 += chars[(n >> 12) & 0x3F];
        base64 += (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
        base64 += (i + 2 < len) ? chars[n & 0x3F] : '=';
        i += 3;
      }
      return "{\"__binary__\":\"" + base64 + "\"}";
    }
    case wef::ValueType::List:
      return SerializeList(value->GetList());
    case wef::ValueType::Dict:
      return SerializeDict(value->GetDict());
    case wef::ValueType::Callback:
      return "{\"__callback__\":\"" + std::to_string(value->GetCallbackId()) + "\"}";
  }
  return "null";
}

inline wef::ValuePtr Parse(const char*& p);

inline void SkipWhitespace(const char*& p) {
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}

inline std::string ParseString(const char*& p) {
  std::string result;
  ++p;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p + 1)) {
      ++p;
      switch (*p) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          if (p[1] && p[2] && p[3] && p[4]) {
            char hex[5] = {p[1], p[2], p[3], p[4], 0};
            int codepoint = (int)strtol(hex, nullptr, 16);
            if (codepoint < 0x80) {
              result += (char)codepoint;
            } else if (codepoint < 0x800) {
              result += (char)(0xC0 | (codepoint >> 6));
              result += (char)(0x80 | (codepoint & 0x3F));
            } else {
              result += (char)(0xE0 | (codepoint >> 12));
              result += (char)(0x80 | ((codepoint >> 6) & 0x3F));
              result += (char)(0x80 | (codepoint & 0x3F));
            }
            p += 4;
          }
          break;
        }
        default: result += *p;
      }
    } else {
      result += *p;
    }
    ++p;
  }
  if (*p == '"') ++p;
  return result;
}

inline wef::ValuePtr ParseArray(const char*& p) {
  auto list = wef::Value::List();
  ++p;
  SkipWhitespace(p);
  while (*p && *p != ']') {
    list->GetList().push_back(Parse(p));
    SkipWhitespace(p);
    if (*p == ',') ++p;
    SkipWhitespace(p);
  }
  if (*p == ']') ++p;
  return list;
}

inline wef::ValuePtr ParseObject(const char*& p) {
  auto dict = wef::Value::Dict();
  ++p;
  SkipWhitespace(p);
  while (*p && *p != '}') {
    SkipWhitespace(p);
    if (*p != '"') break;
    std::string key = ParseString(p);
    SkipWhitespace(p);
    if (*p == ':') ++p;
    SkipWhitespace(p);
    dict->GetDict()[key] = Parse(p);
    SkipWhitespace(p);
    if (*p == ',') ++p;
    SkipWhitespace(p);
  }
  if (*p == '}') ++p;

  const auto& d = dict->GetDict();
  auto it = d.find("__callback__");
  if (it != d.end() && it->second->IsString()) {
    uint64_t id = std::stoull(it->second->GetString());
    return wef::Value::Callback(id);
  }
  it = d.find("__binary__");
  if (it != d.end() && it->second->IsString()) {
    const std::string& base64 = it->second->GetString();
    std::vector<uint8_t> data;
    static const int decode[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
      52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
      -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
      15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
      -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
      41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    int val = 0, bits = -8;
    for (char c : base64) {
      if (c == '=') break;
      int d = decode[(unsigned char)c];
      if (d < 0) continue;
      val = (val << 6) | d;
      bits += 6;
      if (bits >= 0) {
        data.push_back((val >> bits) & 0xFF);
        bits -= 8;
      }
    }
    return wef::Value::Binary(data.data(), data.size());
  }

  return dict;
}

inline wef::ValuePtr Parse(const char*& p) {
  SkipWhitespace(p);
  if (!*p) return wef::Value::Null();

  if (*p == 'n' && strncmp(p, "null", 4) == 0) {
    p += 4;
    return wef::Value::Null();
  }
  if (*p == 't' && strncmp(p, "true", 4) == 0) {
    p += 4;
    return wef::Value::Bool(true);
  }
  if (*p == 'f' && strncmp(p, "false", 5) == 0) {
    p += 5;
    return wef::Value::Bool(false);
  }
  if (*p == '"') {
    return wef::Value::String(ParseString(p));
  }
  if (*p == '[') {
    return ParseArray(p);
  }
  if (*p == '{') {
    return ParseObject(p);
  }
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    char* end;
    double d = strtod(p, &end);
    bool isInt = true;
    for (const char* c = p; c < end; ++c) {
      if (*c == '.' || *c == 'e' || *c == 'E') {
        isInt = false;
        break;
      }
    }
    p = end;
    if (isInt && d >= INT_MIN && d <= INT_MAX) {
      return wef::Value::Int((int)d);
    }
    return wef::Value::Double(d);
  }

  return wef::Value::Null();
}

inline wef::ValuePtr ParseJson(const std::string& json) {
  const char* p = json.c_str();
  return Parse(p);
}

} // namespace json

#endif // WEF_JSON_H_
