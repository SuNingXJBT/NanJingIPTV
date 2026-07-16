#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <clocale>
#else
#include <unistd.h>
#endif

// GBK 转 UTF-8
#ifdef _WIN32
std::string GBKToUTF8(const std::string& gbk_str) {
    if (gbk_str.empty()) return "";
    
    // 获取需要的缓冲区大小
    int wide_len = MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, NULL, 0);
    if (wide_len <= 0) return "";
    
    std::wstring wide_str(wide_len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, &wide_str[0], wide_len);
    
    // 转换为 UTF-8
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, NULL, 0, NULL, NULL);
    if (utf8_len <= 0) return "";
    
    std::string utf8_str(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, &utf8_str[0], utf8_len, NULL, NULL);
    
    // 移除结尾的空字符
    if (!utf8_str.empty() && utf8_str.back() == '\0') {
        utf8_str.pop_back();
    }
    
    return utf8_str;
}

// UTF-8 转 GBK (用于输出到控制台)
std::string UTF8ToGBK(const std::string& utf8_str) {
    return utf8_str;  // 这个函数是多余的，main里面已经设置了按照UTF8处理、显示
}
#endif

struct Programme {
    std::string channel;
    std::string start;      // 原始时间字符串，如 "20260512013800 +0800"
    std::string stop;       // 原始时间字符串，如 "20260512031600 +0800"
    std::string title;
    std::string desc;
    std::string date;
};

struct Channel {
    std::string id;
    std::string name;
    std::string catchup_source;
    bool has_catchup;
};

class EPGSearch {
private:
    std::map<std::string, Channel> channels;
    std::vector<Programme> programmes;

    std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\n\r");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\n\r");
        return str.substr(first, last - first + 1);
    }

    // 执行系统命令并获取输出
    std::string execCommand(const std::string& cmd) {
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) {
            return "";
        }
        char buffer[8192];
        while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            result += buffer;
        }
        return result;
    }

    // 检查命令是否存在
    bool commandExists(const std::string& cmd) {
        std::string checkCmd;
#ifdef _WIN32
        checkCmd = "where " + cmd + " > nul 2>&1";
#else
        checkCmd = "which " + cmd + " > /dev/null 2>&1";
#endif
        return system(checkCmd.c_str()) == 0;
    }

    // 下载并解压 XML 文件
    bool downloadAndDecompressXML(const std::string& url, const std::string& output_file) {
        std::cout << "正在从 " << url << " 下载 EPG 数据..." << std::endl;
        
        // 检查必要的命令
        if (!commandExists("curl")) {
            std::cerr << "错误: 未找到 curl 命令，请先安装 curl" << std::endl;
            return false;
        }
        
        if (!commandExists("gunzip")) {
            std::cerr << "错误: 未找到 gunzip 命令，请先安装 gzip" << std::endl;
            return false;
        }
        
        // 临时压缩文件
        std::string temp_gz_file = output_file + ".tmp.gz";
        
        // 使用 curl 下载 .gz 文件
        std::string curl_cmd = "curl -s -L -o \"" + temp_gz_file + "\" \"" + url + "\"";
        std::cout << "下载中..." << std::endl;
        
        int ret = system(curl_cmd.c_str());
        if (ret != 0) {
            std::cerr << "下载失败，curl 返回错误码: " << ret << std::endl;
            return false;
        }
        
        // 检查下载的文件是否存在且不为空
        std::ifstream test_file(temp_gz_file, std::ios::binary | std::ios::ate);
        if (!test_file.is_open()) {
            std::cerr << "下载文件创建失败" << std::endl;
            return false;
        }
        
        std::streamsize size = test_file.tellg();
        test_file.close();
        
        if (size == 0) {
            std::cerr << "下载的文件为空" << std::endl;
            return false;
        }
        
        std::cout << "下载完成，文件大小: " << size << " 字节" << std::endl;
        std::cout << "正在解压..." << std::endl;
        
        // 使用 gunzip 解压
        std::string gunzip_cmd;
#ifdef _WIN32
        // Windows 下可能需要使用 gzip -d 或 7z
        if (commandExists("gzip")) {
            gunzip_cmd = "gzip -d -c \"" + temp_gz_file + "\" > \"" + output_file + "\"";
        } else if (commandExists("7z")) {
            gunzip_cmd = "7z x -so \"" + temp_gz_file + "\" > \"" + output_file + "\"";
        } else {
            std::cerr << "错误: 未找到 gzip 或 7z 命令" << std::endl;
            remove(temp_gz_file.c_str());
            return false;
        }
#else
        gunzip_cmd = "gunzip -c \"" + temp_gz_file + "\" > \"" + output_file + "\"";
#endif
        
        ret = system(gunzip_cmd.c_str());
        if (ret != 0) {
            std::cerr << "解压失败" << std::endl;
            remove(temp_gz_file.c_str());
            return false;
        }
        
        // 删除临时压缩文件
        remove(temp_gz_file.c_str());
        
        // 验证输出文件
        std::ifstream out_file(output_file);
        if (!out_file.is_open()) {
            std::cerr << "解压后的文件无法打开" << std::endl;
            return false;
        }
        
        out_file.seekg(0, std::ios::end);
        size = out_file.tellg();
        out_file.close();
        
        std::cout << "解压完成，XML 文件大小: " << size << " 字节" << std::endl;
        return true;
    }

    // 解析XML属性
    std::string getAttribute(const std::string& tag, const std::string& attr) {
        std::regex re(attr + "=\"([^\"]*)\"");
        std::smatch match;
        if (std::regex_search(tag, match, re)) {
            return match[1].str();
        }
        return "";
    }

    // 获取标签内容
    std::string getTagContent(const std::string& line, const std::string& tag) {
        std::regex re("<" + tag + "[^>]*>([^<]*)</" + tag + ">");
        std::smatch match;
        if (std::regex_search(line, match, re)) {
            return match[1].str();
        }
        return "";
    }

    // 将带时区的时间字符串转换为 UTC 时间字符串 (yyyyMMddHHmmss)
    // 输入格式: "20260512013800 +0800" 或 "20260512013800"
    // 输出格式: "20260511133800" (UTC时间)
    std::string convertToUTC(const std::string& time_str) {
        if (time_str.empty()) return "";
        
        std::string datetime_part = time_str;
        int tz_offset = 0;
        
        // 检查是否包含时区信息
        std::smatch tz_match;
        std::regex tz_regex(R"((\d{14})\s+([+-])(\d{2})(\d{2}))");
        if (std::regex_search(time_str, tz_match, tz_regex)) {
            datetime_part = tz_match[1].str();
            int sign = (tz_match[2].str() == "+") ? 1 : -1;
            int tz_hour = std::stoi(tz_match[3].str());
            int tz_min = std::stoi(tz_match[4].str());
            tz_offset = sign * (tz_hour * 60 + tz_min);
        } else if (time_str.length() >= 14) {
            datetime_part = time_str.substr(0, 14);
            // 尝试常见时区，默认 UTC+8（中国标准时间）
            tz_offset = 480; // +0800 转换为分钟
        }
        
        if (datetime_part.length() < 14) return "";
        
        // 解析原始时间 (视为本地时间，即包含时区偏移的时间)
        int year = std::stoi(datetime_part.substr(0, 4));
        int month = std::stoi(datetime_part.substr(4, 2));
        int day = std::stoi(datetime_part.substr(6, 2));
        int hour = std::stoi(datetime_part.substr(8, 2));
        int minute = std::stoi(datetime_part.substr(10, 2));
        int second = std::stoi(datetime_part.substr(12, 2));
        
        // 转换为 UTC（减去时区偏移）
        // 使用简单的算术方法，避免复杂的日期库
        
        // 转换为自 1970-01-01 00:00:00 UTC 的分钟数
        // 注意：这是一个简化的计算，不考虑闰秒等复杂情况
        struct tm tm_info = {0};
        tm_info.tm_year = year - 1900;
        tm_info.tm_mon = month - 1;
        tm_info.tm_mday = day;
        tm_info.tm_hour = hour;
        tm_info.tm_min = minute;
        tm_info.tm_sec = second;
        
        // 使用 timegm 或 _mkgmtime 将本地时间转换为 UTC 时间戳
        // 注意：这里 tm 被视为 UTC，然后我们用 tz_offset 调整
        time_t utc_timestamp;
#ifdef _WIN32
        utc_timestamp = _mkgmtime(&tm_info);
#else
        utc_timestamp = timegm(&tm_info);
#endif
        
        if (utc_timestamp == -1) return "";
        
        // 减去时区偏移（分钟转秒）
        utc_timestamp -= tz_offset * 60;
        
        // 转换回 UTC 时间
        struct tm utc_tm;
#ifdef _WIN32
        gmtime_s(&utc_tm, &utc_timestamp);
#else
        gmtime_r(&utc_timestamp, &utc_tm);
#endif
        
        // 格式化为 yyyyMMddHHmmss
        std::stringstream ss;
        ss << std::setfill('0')
           << std::setw(4) << (utc_tm.tm_year + 1900)
           << std::setw(2) << (utc_tm.tm_mon + 1)
           << std::setw(2) << utc_tm.tm_mday
           << std::setw(2) << utc_tm.tm_hour
           << std::setw(2) << utc_tm.tm_min
           << std::setw(2) << utc_tm.tm_sec;
        
        return ss.str();
    }

    // 解析EPG XML文件
    bool parseEPGXML(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "无法打开文件: " << filename << std::endl;
            return false;
        }

        std::string line;
        Programme current_prog;
        bool in_programme = false;
        
        // 先解析频道信息
        file.clear();
        file.seekg(0);
        while (std::getline(file, line)) {
            if (line.find("<channel") != std::string::npos) {
                std::string id = getAttribute(line, "id");
                if (!id.empty()) {
                    Channel ch;
                    ch.id = id;
                    ch.has_catchup = false;
                    channels[id] = ch;
                }
            } else if (line.find("<display-name") != std::string::npos) {
                std::string name = getTagContent(line, "display-name");
                if (!name.empty()) {
                    for (auto& ch : channels) {
                        if (ch.second.name.empty()) {
                            ch.second.name = name;
                            break;
                        }
                    }
                }
            }
        }

        // 解析节目信息
        file.clear();
        file.seekg(0);
        while (std::getline(file, line)) {
            if (line.find("<programme") != std::string::npos) {
                current_prog.channel = getAttribute(line, "channel");
                current_prog.start = getAttribute(line, "start");
                current_prog.stop = getAttribute(line, "stop");
                in_programme = true;
            } else if (in_programme && line.find("<title") != std::string::npos) {
                current_prog.title = getTagContent(line, "title");
            } else if (in_programme && line.find("<desc") != std::string::npos) {
                current_prog.desc = getTagContent(line, "desc");
            } else if (in_programme && line.find("<date") != std::string::npos) {
                current_prog.date = getTagContent(line, "date");
            } else if (in_programme && line.find("</programme>") != std::string::npos) {
                programmes.push_back(current_prog);
                current_prog = Programme();
                in_programme = false;
            }
        }

        std::cout << "解析完成: " << channels.size() << " 个频道, " 
                  << programmes.size() << " 个节目" << std::endl;
        return true;
    }

    // 解析M3U文件，提取回放链接
    bool parseM3U(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "无法打开文件: " << filename << std::endl;
            return false;
        }

        std::string line;
        std::string current_channel_name;
        std::string current_catchup_source;
        
        while (std::getline(file, line)) {
            if (line.find("#EXTINF") != std::string::npos) {
                current_channel_name = "";
                current_catchup_source = "";
                
                // 提取 tvg-name
                std::regex name_re("tvg-name=\"([^\"]*)\"");
                std::smatch name_match;
                if (std::regex_search(line, name_match, name_re)) {
                    current_channel_name = name_match[1].str();
                }
                
                // 提取 tvg-id
                std::regex id_re("tvg-id=\"([^\"]*)\"");
                std::smatch id_match;
                std::string tvg_id;
                if (std::regex_search(line, id_match, id_re)) {
                    tvg_id = id_match[1].str();
                }
                
                // 提取 catchup-source
                std::regex source_re("catchup-source=\"([^\"]*)\"");
                std::smatch source_match;
                if (std::regex_search(line, source_match, source_re)) {
                    current_catchup_source = source_match[1].str();
                }
                
                // 匹配到tvg-id时更新channel信息
                if (!tvg_id.empty() && channels.find(tvg_id) != channels.end()) {
                    channels[tvg_id].catchup_source = current_catchup_source;
                    channels[tvg_id].has_catchup = !current_catchup_source.empty();
                } else if (!current_channel_name.empty()) {
                    // 尝试按名称匹配
                    for (auto& ch : channels) {
                        if (ch.second.name == current_channel_name) {
                            ch.second.catchup_source = current_catchup_source;
                            ch.second.has_catchup = !current_catchup_source.empty();
                            break;
                        }
                    }
                }
            }
        }
        
        return true;
    }

    // 组装回放链接 - 使用 UTC 时间
    std::string buildCatchupURL(const std::string& source_template, 
                                 const std::string& start, 
                                 const std::string& stop) {
        if (source_template.empty()) return "";
        
        std::string url = source_template;
        
        // 转换为 UTC 时间
        std::string start_utc = convertToUTC(start);
        std::string stop_utc = convertToUTC(stop);
        
        if (start_utc.empty() || stop_utc.empty()) {
            std::cerr << "时间转换失败: start=" << start << ", stop=" << stop << std::endl;
            return "";
        }
        
        // 替换开始时间占位符（支持 ${...} 和 {...} 两种格式，以及各种常见变量名）
        // 常见格式: {utc:YmdHis}, ${start}, {b}, etc.
        std::regex start_regex1(R"(\$\{?\(b\)yyyyMMddHHmmss:utc\}?)");
        std::regex start_regex2(R"(\$\{?start\}?)");
        std::regex start_regex3(R"(\$\{?utc:YmdHis\}?)");
        std::regex start_regex4(R"(\$\{?\(b\)\}?)");
        
        url = std::regex_replace(url, start_regex1, start_utc);
        url = std::regex_replace(url, start_regex2, start_utc);
        url = std::regex_replace(url, start_regex3, start_utc);
        url = std::regex_replace(url, start_regex4, start_utc);
        
        // 替换结束时间占位符
        std::regex stop_regex1(R"(\$\{?\(e\)yyyyMMddHHmmss:utc\}?)");
        std::regex stop_regex2(R"(\$\{?end\}?)");
        std::regex stop_regex3(R"(\$\{?\(e\)\}?)");
        
        url = std::regex_replace(url, stop_regex1, stop_utc);
        url = std::regex_replace(url, stop_regex2, stop_utc);
        url = std::regex_replace(url, stop_regex3, stop_utc);
        
        // 通用替换：前后缀识别
        std::regex generic_regex(R"(\$\{?[^}]+\}?)");
        if (url.find("${") != std::string::npos || url.find("{") != std::string::npos) {
            // 如果还有未替换的占位符，尝试按模式替换
            std::string temp;
            size_t pos = 0;
            while (pos < url.length()) {
                if (url[pos] == '$' && pos + 1 < url.length() && url[pos + 1] == '{') {
                    size_t end = url.find('}', pos);
                    if (end != std::string::npos) {
                        std::string placeholder = url.substr(pos, end - pos + 1);
                        if (placeholder.find("b") != std::string::npos || 
                            placeholder.find("start") != std::string::npos) {
                            url.replace(pos, placeholder.length(), start_utc);
                        } else if (placeholder.find("e") != std::string::npos ||
                                   placeholder.find("end") != std::string::npos) {
                            url.replace(pos, placeholder.length(), stop_utc);
                        } else {
                            pos++;
                        }
                    } else {
                        pos++;
                    }
                } else if (url[pos] == '{' && !(pos > 0 && url[pos-1] == '$')) {
                    size_t end = url.find('}', pos);
                    if (end != std::string::npos) {
                        std::string placeholder = url.substr(pos, end - pos + 1);
                        if (placeholder.find("b") != std::string::npos || 
                            placeholder.find("start") != std::string::npos) {
                            url.replace(pos, placeholder.length(), start_utc);
                        } else if (placeholder.find("e") != std::string::npos ||
                                   placeholder.find("end") != std::string::npos) {
                            url.replace(pos, placeholder.length(), stop_utc);
                        } else {
                            pos++;
                        }
                    } else {
                        pos++;
                    }
                } else {
                    pos++;
                }
            }
        }
        
        return url;
    }

public:
    bool initialize(const std::string& epg_file, const std::string& m3u_file, bool download_epg = false, const std::string& epg_url = "") {
        std::string actual_epg_file = epg_file;
        download_epg = TRUE;		//直接就按照下载节目单处理
        // 如果需要下载或指定了URL，则下载EPG
        if (download_epg || !epg_url.empty()) {
            std::string url = epg_url.empty() ? "http://e.erw.cc/all.xml.gz" : epg_url;
            
            // 生成临时文件名
            //std::string temp_xml_file = "temp_epg_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".xml";
            std::string temp_xml_file = "temp_epg.xml";
            if (downloadAndDecompressXML(url, temp_xml_file)) {
                actual_epg_file = temp_xml_file;
                std::cout << "EPG 数据下载并解压成功" << std::endl;
            } else {
                std::cerr << "下载失败，尝试使用本地文件..." << std::endl;
                // 下载失败，尝试使用本地文件
                actual_epg_file = epg_file;
            }
        }
        
        std::cout << "正在解析EPG文件: " << actual_epg_file << std::endl;
        if (!parseEPGXML(actual_epg_file)) {
            // 如果是临时文件，清理它
            if (actual_epg_file.find("temp_epg_") != std::string::npos) {
//                remove(actual_epg_file.c_str());
            }
            return false;
        }
        
        // 如果是临时文件，解析完成后删除
        if (actual_epg_file.find("temp_epg_") != std::string::npos) {
  //          remove(actual_epg_file.c_str());
//            std::cout << "临时文件已清理" << std::endl;
        }
        
        std::cout << "正在解析M3U文件..." << std::endl;
        if (!parseM3U(m3u_file)) return false;
        
        int catchup_count = 0;
        for (const auto& ch : channels) {
            if (ch.second.has_catchup) catchup_count++;
        }
        std::cout << "找到 " << catchup_count << " 个支持回放的频道" << std::endl;
        
        return true;
    }

    void search(const std::string& keyword) {
        std::vector<std::pair<Programme, Channel>> results;
        
        for (const auto& prog : programmes) {
            if (prog.title.find(keyword) != std::string::npos ||
                prog.desc.find(keyword) != std::string::npos) {
                
                auto it = channels.find(prog.channel);
                if (it != channels.end()) {
                    results.push_back({prog, it->second});
                }
            }
        }
        
        if (results.empty()) {
            std::cout << "\n未找到包含 \"" << keyword << "\" 的节目" << std::endl;
            return;
        }
        
        std::cout << "\n找到 " << results.size() << " 个包含 \"" << keyword << "\" 的节目:\n" 
                  << std::string(80, '=') << std::endl;
        
        for (size_t i = 0; i < results.size(); i++) {
            const auto& prog = results[i].first;
            const auto& ch = results[i].second;
            
            // 输出时转换为GBK以便在Windows控制台正确显示
#ifdef _WIN32
            std::string display_name = UTF8ToGBK(ch.name);
            std::string display_title = UTF8ToGBK(prog.title);
            std::string display_desc = UTF8ToGBK(prog.desc);
#else
            std::string display_name = ch.name;
            std::string display_title = prog.title;
            std::string display_desc = prog.desc;
#endif
            
            std::cout << "\n[" << (i + 1) << "] " << display_name << std::endl;
            std::cout << "    节目: " << display_title << std::endl;
            if (!prog.desc.empty()) {
                std::cout << "    描述: " << display_desc << std::endl;
            }
            
            // 显示原始时间 (本地时间)
            std::string start_local = prog.start.substr(0, 14);
            std::string stop_local = prog.stop.substr(0, 14);
            std::cout << "    本地时间: " << start_local.substr(0, 4) << "-" << start_local.substr(4, 2) 
                      << "-" << start_local.substr(6, 2) << " " << start_local.substr(8, 2) 
                      << ":" << start_local.substr(10, 2) << ":" << start_local.substr(12, 2);
            std::cout << " 至 ";
            std::cout << stop_local.substr(8, 2) << ":" << stop_local.substr(10, 2) 
                      << ":" << stop_local.substr(12, 2);
            
            // 显示时区信息
            size_t tz_pos1 = prog.start.find("+");
            size_t tz_pos2 = prog.start.find("-", 1);
            if (tz_pos1 != std::string::npos || tz_pos2 != std::string::npos) {
                std::string tz = (tz_pos1 != std::string::npos) ? prog.start.substr(tz_pos1) : prog.stop.substr(tz_pos2);
                std::cout << " (" << tz << ")";
            }
            std::cout << std::endl;
            
            // 显示 UTC 时间
            std::string start_utc = convertToUTC(prog.start);
            std::string stop_utc = convertToUTC(prog.stop);
            if (!start_utc.empty() && !stop_utc.empty()) {
                std::cout << "    UTC 时间: " << start_utc.substr(0, 4) << "-" << start_utc.substr(4, 2) 
                          << "-" << start_utc.substr(6, 2) << " " << start_utc.substr(8, 2) 
                          << ":" << start_utc.substr(10, 2) << ":" << start_utc.substr(12, 2);
                std::cout << " 至 ";
                std::cout << stop_utc.substr(8, 2) << ":" << stop_utc.substr(10, 2) 
                          << ":" << stop_utc.substr(12, 2) << " UTC" << std::endl;
            }
            
            if (ch.has_catchup && !ch.catchup_source.empty()) {
                std::string url = buildCatchupURL(ch.catchup_source, prog.start, prog.stop);
                if (!url.empty()) {
                    std::cout << "    回放链接: " << url << std::endl;
                } else {
                    std::cout << "    回放链接: (无法生成)" << std::endl;
                }
            } else {
                std::cout << "    回放链接: (该频道不支持回放)" << std::endl;
            }
        }
        
        std::cout << "\n" << std::string(80, '=') << std::endl;
    }

    void listChannels() {
        std::cout << "\n支持回放的频道列表:\n" << std::string(60, '-') << std::endl;
        for (const auto& ch : channels) {
            if (ch.second.has_catchup) {
#ifdef _WIN32
                std::string display_name = UTF8ToGBK(ch.second.name);
#else
                std::string display_name = ch.second.name;
#endif
                std::cout << "  " << display_name
                          << " (ID: " << ch.second.id << ")" << std::endl;
            }
        }
    }
};

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 设置控制台为UTF-8编码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // 设置locale以支持中文
    std::setlocale(LC_ALL, "zh_CN.UTF-8");
#endif
    
    std::cout << "IPTV EPG 节目搜索工具 (支持时区转换)\n" << std::string(50, '=') << std::endl;
    
    std::string epg_file = "epgpw_cn.xml";
    std::string m3u_file = "mym3u8.txt";
    std::string keyword;
    bool download_epg = false;
    std::string epg_url = "";
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-e" && i + 1 < argc) {
            epg_file = argv[++i];
        } else if (arg == "-m" && i + 1 < argc) {
            m3u_file = argv[++i];
        } else if (arg == "-k" && i + 1 < argc) {
#ifdef _WIN32
            // 命令行参数中的中文是GBK编码，转换为UTF-8
            keyword = GBKToUTF8(argv[++i]);
#else
            keyword = argv[++i];
#endif
        } else if (arg == "-u" && i + 1 < argc) {
            epg_url = argv[++i];
            download_epg = true;
        } else if (arg == "--download") {
            download_epg = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "\n用法: " << argv[0] << " [选项]\n"
                      << "  -e <file>    指定EPG XML文件 (默认: epgpw_cn.xml)\n"
                      << "  -m <file>    指定M3U文件 (默认: mym3u8.txt)\n"
                      << "  -k <keyword> 搜索关键词\n"
                      << "  -u <url>     从指定URL下载EPG (支持.gz压缩文件)\n"
                      << "  --download   使用默认URL下载EPG (http://e.erw.cc/all.xml.gz)\n"
                      << "  -l           列出支持回放的频道\n"
                      << "  -h, --help   显示帮助信息\n"
                      << "\n示例:\n"
                      << "  " << argv[0] << " --download -k \"英超\"\n"
                      << "  " << argv[0] << " -u http://e.erw.cc/all.xml.gz -k \"新闻联播\"\n"
                      << "  " << argv[0] << " -k \"足球\"\n"
                      << "  " << argv[0] << " -l\n"
                      << "\n说明: \n"
                      << "  - 使用 --download 或 -u 参数会自动从网络下载最新的EPG数据\n"
                      << "  - EPG时间戳中的 +0800 时区会自动转换为 UTC 时间用于回放链接\n"
                      << "  - 需要系统已安装 curl 和 gunzip/gzip 命令\n";
            return 0;
        } else if (arg == "-l") {
            EPGSearch searcher;
            if (searcher.initialize(epg_file, m3u_file, download_epg, epg_url)) {
                searcher.listChannels();
            }
            return 0;
        }
    }
    
    if (keyword.empty()) {
        std::cout << "\n请输入搜索关键词: ";
        std::string input;
        std::getline(std::cin, input);
#ifdef _WIN32
        keyword = input;
#else
        keyword = input;
#endif
    }
    
    if (keyword.empty()) {
        std::cerr << "错误: 未提供搜索关键词" << std::endl;
        return 1;
    }
    
    EPGSearch searcher;
    if (!searcher.initialize(epg_file, m3u_file, download_epg, epg_url)) {
        std::cerr << "初始化失败" << std::endl;
        return 1;
    }
    
    searcher.search(keyword);
    
    return 0;
}