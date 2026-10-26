#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <variant>
#include <stdexcept>
#include <cstdint>
#include <charconv>

// Small, bounded JSON codec. Numbers retain their original digits (account IDs
// and generations must never be rounded through floating point).
namespace ta {
struct Number { std::string text; };
struct Json {
    using Object=std::map<std::string,Json>;
    using Array=std::vector<Json>;
    std::variant<std::nullptr_t,bool,Number,std::string,Array,Object> v=nullptr;
    Json()=default;
    Json(const char* s):v(std::string(s)){}
    Json(std::string s):v(std::move(s)){}
    Json(bool b):v(b){}
    Json(Object o):v(std::move(o)){}
    Json(Array a):v(std::move(a)){}
    static Json num(int64_t n) { Json j;j.v=Number{std::to_string(n)};return j; }
    bool object() const{return std::holds_alternative<Object>(v);}
    bool array() const{return std::holds_alternative<Array>(v);}
    bool null() const{return std::holds_alternative<std::nullptr_t>(v);}
    bool has(const std::string& k)const {auto p=std::get_if<Object>(&v);return p&&p->count(k);}
    const Json& operator[](const std::string& k)const {static const Json empty;auto p=std::get_if<Object>(&v);if(!p)return empty;auto it=p->find(k);return it==p->end()?empty:it->second;}
    Json& operator[](const std::string& k){if(!object())v=Object{};return std::get<Object>(v)[k];}
    std::string str(std::string fallback={})const {if(auto p=std::get_if<std::string>(&v))return *p;return fallback;}
    int64_t integer(int64_t fallback=0)const {auto p=std::get_if<Number>(&v);if(!p)return fallback;int64_t n=0;auto [e,err]=std::from_chars(p->text.data(),p->text.data()+p->text.size(),n);return err==std::errc()&&e==p->text.data()+p->text.size()?n:fallback;}
    bool boolean(bool fallback=false)const {if(auto p=std::get_if<bool>(&v))return *p;return fallback;}
    static void utf8(std::string& out,uint32_t cp){
        if(cp<0x80)out+=char(cp);
        else if(cp<0x800){out+=char(0xc0|(cp>>6));out+=char(0x80|(cp&63));}
        else if(cp<0x10000){out+=char(0xe0|(cp>>12));out+=char(0x80|((cp>>6)&63));out+=char(0x80|(cp&63));}
        else{out+=char(0xf0|(cp>>18));out+=char(0x80|((cp>>12)&63));out+=char(0x80|((cp>>6)&63));out+=char(0x80|(cp&63));}
    }
    static Json parse(std::string_view s) {
        struct Parser {
            std::string_view s;size_t p=0;
            [[noreturn]]void fail(){throw std::runtime_error("Invalid JSON");}
            void ws(){while(p<s.size()&&(s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t'))++p;}
            unsigned hex(){if(p+4>s.size())fail();unsigned n=0;for(int i=0;i<4;++i){char c=s[p++];n<<=4;if(c>='0'&&c<='9')n+=c-'0';else if(c>='a'&&c<='f')n+=10+c-'a';else if(c>='A'&&c<='F')n+=10+c-'A';else fail();}return n;}
            std::string text(){
                if(p>=s.size()||s[p++]!='"')fail();
                std::string out;
                while(p<s.size()) {unsigned char c=s[p++];if(c=='"')return out;if(c<32)fail();if(c!='\\'){out+=char(c);continue;}
                    if(p>=s.size())fail();
                    char e=s[p++];switch(e){case '"':case '\\':case '/':out+=e;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
                    case 'u':{unsigned cp=hex();if(cp>=0xd800&&cp<=0xdbff){if(p+2>s.size()||s[p++]!='\\'||s[p++]!='u')fail();unsigned lo=hex();if(lo<0xdc00||lo>0xdfff)fail();cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);}else if(cp>=0xdc00&&cp<=0xdfff)fail();Json::utf8(out,cp);break;}default:fail();}
                }fail();
            }
            Json value(int depth){
                if(depth>64)fail();
                ws();if(p>=s.size())fail();char c=s[p];
                if(c=='"')return Json(text());
                if(c=='{'){++p;Object o;ws();if(p<s.size()&&s[p]=='}'){++p;return Json(o);}for(;;){ws();std::string k=text();ws();if(p>=s.size()||s[p++]!=':')fail();if(o.count(k))fail();o.emplace(std::move(k),value(depth+1));ws();if(p>=s.size())fail();char e=s[p++];if(e=='}')break;if(e!=',')fail();}return Json(o);}
                if(c=='['){++p;Array a;ws();if(p<s.size()&&s[p]==']'){++p;return Json(a);}for(;;){a.push_back(value(depth+1));ws();if(p>=s.size())fail();char e=s[p++];if(e==']')break;if(e!=',')fail();}return Json(a);}
                for(auto word:{"true","false","null"}){std::string_view w(word);if(s.substr(p,w.size())==w){p+=w.size();if(w=="null")return Json();return Json(w=="true");}}
                size_t a=p;if(s[p]=='-')++p;if(p>=s.size())fail();if(s[p]=='0')++p;else{if(s[p]<'1'||s[p]>'9')fail();while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;}
                if(p<s.size()&&s[p]=='.'){++p;size_t b=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;if(p==b)fail();}
                if(p<s.size()&&(s[p]=='e'||s[p]=='E')){++p;if(p<s.size()&&(s[p]=='+'||s[p]=='-'))++p;size_t b=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;if(p==b)fail();}
                Json j;j.v=Number{std::string(s.substr(a,p-a))};return j;
            }
        };
        if(s.size()>32*1024*1024)throw std::runtime_error("JSON too large");
        if(s.substr(0,3)=="\xef\xbb\xbf")s.remove_prefix(3);
        Parser p{s};Json j=p.value(0);p.ws();if(p.p!=s.size())p.fail();return j;
    }
    std::string dump()const {
        auto quote=[](const std::string& s){std::string out="\"";const char* h="0123456789abcdef";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=h[c>>4];out+=h[c&15];}else out+=char(c);}return out+'"';};
        if(null())return "null";
        if(auto p=std::get_if<bool>(&v))return *p?"true":"false";
        if(auto p=std::get_if<Number>(&v))return p->text;
        if(auto p=std::get_if<std::string>(&v))return quote(*p);
        std::string out;bool first=true;if(auto a=std::get_if<Array>(&v)){out="[";for(auto& j:*a){if(!first)out+=',';first=false;out+=j.dump();}return out+']';}
        out="{";for(auto& [k,j]:std::get<Object>(v)){if(!first)out+=',';first=false;out+=quote(k)+':'+j.dump();}return out+'}';
    }
};
}
