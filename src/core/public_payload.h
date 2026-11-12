#pragma once
#include "core/json.h"
#include "core/strings.h"
#include <map>

namespace ng {
inline void label_sessions(json& value,const std::map<std::string,std::string>& labels) {
    if (value.is_object()) {
        for (const auto* key:{"parent_id","session_id","strategy_key","id"}) {
            auto it=value.find(key);
            if (it!=value.end() && it->is_string()) {
                auto label=labels.find(it->get<std::string>());
                if (label!=labels.end()) { value["display_id"]=label->second;break; }
            }
        }
        for (auto it=value.begin();it!=value.end();++it) label_sessions(it.value(),labels);
    } else if (value.is_array()) for(auto& item:value) label_sessions(item,labels);
}
// Only use at browser boundaries: calculation and persisted algorithm state keep
// their original values. Apply to cached/SSE payloads as well as ordinary APIs.
inline json public_payload(const json& value) {
    if (value.is_object()) {
        json out=json::object();
        for (auto it=value.begin();it!=value.end();++it) {
            if (to_lower(it.key()).find("participation")!=std::string::npos) continue;
            out[it.key()]=public_payload(it.value());
        }
        return out;
    }
    if (value.is_array()) {
        json out=json::array();
        for (const auto& item:value) out.push_back(public_payload(item));
        return out;
    }
    if (value.is_string()) {
        const auto& text=value.get_ref<const std::string&>();
        auto parsed=json::parse(text,nullptr,false);
        if (parsed.is_object() || parsed.is_array()) return json_dump_compact(public_payload(parsed));
        if (to_lower(text).find("participation")!=std::string::npos || text.find("参与率")!=std::string::npos || text.find("參與率")!=std::string::npos)
            return "[private algorithm setting]";
    }
    return value;
}
}
