#pragma once
#include <map>
#include <string>

// Strict, opt-in external tensor semantics. Absence of a sidecar preserves Auto.
struct ModelOutputContract {
    enum class Format { Auto, Yolo8, Yolo5, NmsXYXY };
    Format format=Format::Auto;
    bool anchorsFirst=false, normalized=false, verifiedNms=false;
    static bool ParseText(std::string text,ModelOutputContract& result,std::string& error)
    {
        if(text.compare(0,3,"\xEF\xBB\xBF")==0)text.erase(0,3);
        std::map<std::string,std::string> kv;
        auto trim=[](std::string value){
            auto a=value.find_first_not_of(" \t\r");
            if(a==std::string::npos)return std::string();
            auto b=value.find_last_not_of(" \t\r");
            return value.substr(a,b-a+1);
        };
        for(size_t pos=0;pos<text.size();) {
            auto end=text.find('\n',pos);auto line=trim(text.substr(pos,end==std::string::npos?end:end-pos));
            pos=end==std::string::npos?text.size():end+1;
            if(line.empty() || line[0]=='#')continue;
            auto eq=line.find('=');if(eq==std::string::npos){error="Model contract line requires key=value";return false;}
            auto key=trim(line.substr(0,eq)),value=trim(line.substr(eq+1));
            if(!kv.emplace(key,value).second){error="Duplicate model contract key: "+key;return false;}
        }
        return Parse(kv,result,error);
    }
    std::string Describe()const {
        if(format==Format::Auto)return "auto (heuristic)";
        return std::string(format==Format::NmsXYXY?"nms_xyxy":format==Format::Yolo5?"yolo5":"yolo8")+
            (anchorsFirst?" / anchors_first":" / channels_first")+
            (normalized?" / normalized":" / pixels")+(verifiedNms?" / verified_nms":" / CPU NMS");
    }
    static bool Parse(const std::map<std::string,std::string>& kv,
        ModelOutputContract& result,std::string& error)
    {
        ModelOutputContract c;
        for(const auto& item:kv)
            if(item.first!="format" && item.first!="axes" && item.first!="coordinates" && item.first!="verified_nms") {
                error="Unknown model contract key: "+item.first;return false;
            }
        auto value=[&](const char* key)->std::string{auto i=kv.find(key);return i==kv.end()?std::string():i->second;};
        const auto f=value("format"),axes=value("axes"),coords=value("coordinates"),nms=value("verified_nms");
        if(f=="auto") {
            if(!axes.empty() || !coords.empty() || (!nms.empty() && nms!="0")) {
                error="Auto contract cannot override axes/coordinates/NMS";return false;
            }
        } else {
            if(f=="yolo8")c.format=Format::Yolo8;
            else if(f=="yolo5")c.format=Format::Yolo5;
            else if(f=="nms_xyxy")c.format=Format::NmsXYXY;
            else {error="format must be auto, yolo8, yolo5 or nms_xyxy";return false;}
            if(axes=="anchors_first")c.anchorsFirst=true;
            else if(axes!="channels_first") {error="Explicit contract requires axes";return false;}
            if(coords=="normalized")c.normalized=true;
            else if(coords!="pixels") {error="Explicit contract requires coordinates";return false;}
            if(!nms.empty() && nms!="0" && nms!="1") {error="verified_nms must be 0 or 1";return false;}
            c.verifiedNms=nms=="1";
            if(c.verifiedNms && c.format!=Format::NmsXYXY) {error="verified_nms requires nms_xyxy";return false;}
        }
        result=c;error.clear();return true;
    }
};
