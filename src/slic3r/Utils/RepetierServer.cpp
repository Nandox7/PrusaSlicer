#include "RepetierServer.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/progdlg.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "Http.hpp"


namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

    RepetierServer::RepetierServer(DynamicPrintConfig* config) :
        host(config->opt_string("print_host")),
        apikey(config->opt_string("printhost_apikey")),
        cafile(config->opt_string("printhost_cafile")),
        printername(config->opt_string("printhost_printername"))
    {}

    const char* RepetierServer::get_name() const { return "RepetierServer"; }

    bool RepetierServer::test(wxString& msg) const
    {
        // Since the request is performed synchronously here,
        // it is ok to refer to `msg` from within the closure

        const char* name = get_name();

        bool res = true;
        auto url = make_url("printer/info");

        BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;

        auto http = Http::get(std::move(url));
        set_auth(http);
        http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            msg = format_error(body, error, status);
            })
            .on_complete([&, this](std::string body, unsigned) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;

                try {
                    std::stringstream ss(body);
                    pt::ptree ptree;
                    pt::read_json(ss, ptree);

                    if (!ptree.get_optional<std::string>("version")) {
                        res = false;
                        return;
                    }

                    const auto text = ptree.get_optional<std::string>("name");
                    res = validate_version_text(text);
                    if (!res) {
                        msg = GUI::from_u8((boost::format(_utf8(L("Mismatched type of print host: %s"))) % (text ? *text : "Repetier-Server")).str());
                    }
                }
                catch (const std::exception&) {
                    res = false;
                    msg = "Could not parse server response";
                }
                })
                .perform_sync();

                return res;
    }

    wxString RepetierServer::get_test_ok_msg() const
    {
        return _(L("Connection to RepetierServer works correctly."));
    }

    wxString RepetierServer::get_test_failed_msg(wxString& msg) const
    {
        return GUI::from_u8((boost::format("%s: %s\n\n%s")
            % _utf8(L("Could not connect to RepetierServer"))
            % std::string(msg.ToUTF8())
            % _utf8(L("Note: Repetier-Server version at least 0.92.2 is required."))).str());
    }

    bool RepetierServer::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn) const
    {
        const char* name = get_name();

        const auto upload_filename = upload_data.upload_path.filename();
        const auto upload_parent_path = upload_data.upload_path.parent_path();

        wxString test_msg;
        if (!test(test_msg)) {
            error_fn(std::move(test_msg));
            return false;
        }

        bool res = true;


        // Examples
        // "x-api-key: <Your API>" -F "a=upload" -F "filename=@[output_filepath]" "http://<Your IP>:3344/printer/model/<Your Printer Name>"
        // "x-api-key: <Your API>" -F "a=upload" -F "filename=@[output_filepath]" -F "name=[output_filename]" "http://<Your IP>:3344/printer/job/<Your Printer Name>"
        auto url = make_url((boost::format("/printer/%1%/%2%") % (upload_data.start_print ? "job" : "model") % printername).str());

        BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%")
            % name
            % upload_data.source_path
            % url
            % upload_filename.string()
            % upload_parent_path.string()
            % upload_data.start_print;

        auto http = Http::post(std::move(url));
        set_auth(http);
        http.form_add("a", "upload")
            //.form_add("path", upload_parent_path.string())      // XXX: slashes on windows ???
            .form_add_file("filename", upload_data.source_path.string(), upload_filename.string())
            .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
                })
            .on_error([&](std::string body, std::string error, unsigned status) {
                    BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
                    error_fn(format_error(body, error, status));
                    res = false;
                })
                    .on_progress([&](Http::Progress progress, bool& cancel) {
                    prorgess_fn(std::move(progress), cancel);
                    if (cancel) {
                        // Upload was canceled
                        BOOST_LOG_TRIVIAL(info) << "RepetierSerer: Upload canceled";
                        res = false;
                    }
                        })
                    .perform_sync();

                        return res;
    }

    bool RepetierServer::validate_version_text(const boost::optional<std::string>& version_text) const
    {
        return version_text ? boost::starts_with(*version_text, "Repetier-Server") : true;
    }

    void RepetierServer::set_auth(Http& http) const
    {
        http.header("X-Api-Key", apikey);

        if (!cafile.empty()) {
            http.ca_file(cafile);
        }
    }

    std::string RepetierServer::make_url(const std::string& path) const
    {
        if (host.find("http://") == 0 || host.find("https://") == 0) {
            if (host.back() == '/') {
                return (boost::format("%1%%2%") % host % path).str();
            }
            else {
                return (boost::format("%1%/%2%") % host % path).str();
            }
        }
        else {
            return (boost::format("http://%1%/%2%") % host % path).str();
        }
    }

}