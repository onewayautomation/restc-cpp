
#include "restc-cpp/DataReaderStream.h"
#include "restc-cpp/error.h"
#include "restc-cpp/url_encode.h"
#include "restc-cpp/logging.h"
#include <restc-cpp/typename.h>

using namespace std;

namespace restc_cpp {

DataReaderStream::DataReaderStream(std::unique_ptr<DataReader>&& source)
: source_{move(source)} {
    RESTC_CPP_LOG_TRACE << "DataReaderStream: Chained to "
        << RESTC_CPP_TYPENAME(decltype(*source_));
}

void DataReaderStream::Fetch() {
    if (++curr_ >= end_) {
        auto buf = source_->ReadSome();

        RESTC_CPP_LOG_TRACE << "DataReaderStream::Fetch: Fetched buffer with "
            << boost::asio::buffer_size(buf) << " bytes.";

        const auto bytes = boost::asio::buffer_size(buf);
        if (bytes == 0) {
            RESTC_CPP_LOG_TRACE << "DataReaderStream::Fetch: EOF";
            throw ProtocolException("Fetch(): EOF");
        }
        curr_ = boost::asio::buffer_cast<const char *>(buf);
        end_ = curr_ + boost::asio::buffer_size(buf);
    }
}

boost::asio::const_buffers_1
DataReaderStream::ReadSome() {
    Fetch();

    boost::asio::const_buffers_1 rval = {curr_,
        static_cast<size_t>(end_ - curr_)};
    curr_ = end_;
    RESTC_CPP_LOG_TRACE << "DataReaderStream::ReadSome: Returning buffer with "
        << boost::asio::buffer_size(rval) << " bytes.";

    if (source_->IsEof()) {
        SetEof();
    }

    return rval;
}

boost::asio::const_buffers_1
DataReaderStream::GetData(size_t maxBytes) {
    Fetch();

    const auto diff = end_ - curr_;
    assert(diff >= 0);
    const auto seg_len = std::min<size_t>(maxBytes, diff);
    boost::asio::const_buffers_1 rval = {curr_, seg_len};
    if (seg_len > 0) {
        curr_ += seg_len - 1;
    }

    RESTC_CPP_LOG_TRACE << "DataReaderStream::GetData(" << maxBytes << "): Returning buffer with "
        << boost::asio::buffer_size(rval) << " bytes.";

    return rval;
}


void DataReaderStream::ReadServerResponse(Reply::HttpResponse& response)
{
    string http_1_1{"HTTP/1.1"};
    char ch = {};
    getc_bytes_ = 0;

    // Get HTTP version
    std::string value;
    for(ch = Getc(); ch != ' '; ch = Getc()) {
        value += ch;
        if (value.size() > 16) {
            throw ProtocolException("ReadHeaders(): Too much HTTP version!");
        }
    }
    if (ch != ' ') {
        throw ProtocolException("ReadHeaders(): No space after HTTP version");
    }
    if (value.empty()) {
        throw ProtocolException("ReadHeaders(): No HTTP version");
    }
    if (ciEqLibC()(value, http_1_1)) {
        ; // Do nothing HTTP 1.1 is the default value
    } else {
        throw ProtocolException(
            string("ReadHeaders(): unsupported HTTP version: ")
                + url_encode(value));
    }

    // Get response code
    value.clear();
    for(ch = Getc(); ch != ' '; ch = Getc()) {
        value += ch;
        if (value.size() > 3) {
            throw ProtocolException("ReadHeaders(): Too much HTTP response code!");
        }
    }
    if (value.size() != 3) {
        throw ProtocolException(
            string("ReadHeaders(): Incorrect length of HTTP response code!: ")
            + value);
    }

    response.status_code = stoi(value);

    if (ch != ' ') {
        throw ProtocolException("ReadHeaders(): No space after HTTP response code");
    }

    // Get response text
    value.clear();
    for(ch = Getc(); ch != '\r'; ch = Getc()) {
        value += ch;
        if (value.size() > 256) {
            throw ConstraintException("ReadHeaders(): Too long HTTP response phrase!");
        }
    }

    // Skip CRLF
    assert(ch == '\r');
    ch = Getc();
    if (ch != '\n') {
        throw ProtocolException("ReadHeaders(): No CR/LF after HTTP response phrase!");
    }

    response.reason_phrase = move(value);
    RESTC_CPP_LOG_TRACE << "ReadServerResponse: getc_bytes is " <<  getc_bytes_;

    RESTC_CPP_LOG_TRACE << "HTTP Response: "
        << (response.http_version == Reply::HttpResponse::HttpVersion::HTTP_1_1
            ? "HTTP/1.1" : "???")
        << ' ' << response.status_code
        << ' ' << response.reason_phrase;
}

void DataReaderStream::ReadHeaderLines(const add_header_fn_t& addHeader) {
    while(true) {
        char ch;
        std::string name, value;
        for(ch = Getc(); ch != '\r'; ch = Getc()) {
            if (ch == ' ' || ch == '\t') {
                continue;
            }
            if (ch == ':') {
                value = GetHeaderValue();
                ch = '\n';
                break;
            }
            name += ch;
            if (name.size() > 256) {
                throw ConstraintException("Chunk Trailer: Header name too long!");
            }
        }

        if (ch == '\r') {
            ch = Getc();
        }

        if (ch != '\n') {
            throw ProtocolException("Chunk Trailer: Missing LF after parse!");
        }

        if (name.empty()) {
            if (!value.empty()) {
                throw ProtocolException("Chunk Trailer: Header value without name!");
            }
            RESTC_CPP_LOG_TRACE << "ReadHeaderLines: getc_bytes is " <<  getc_bytes_;
            getc_bytes_ = 0;
            return; // An empty line marks the end of the trailer
        }

        if (++num_headers_ > 256) {
            throw ConstraintException("Chunk Trailer: Too many lines in header!");
        }

        RESTC_CPP_LOG_TRACE << name << ": " << value;
        addHeader(move(name), move(value));
        name.clear();
        value.clear();
    }
}

std::string DataReaderStream::GetHeaderValue() {
    std::string value;
    char ch;

    while(true) {
        for (ch = Getc(); ch == ' ' || ch == '\t'; ch = Getc())
            ; // skip space

        for (; ch != '\r'; ch = Getc()) {
            value += ch;
            if (value.size() > (1024 * 4)) {
                throw ConstraintException("Chunk Trailer: Header value too long!");
            }
        }

        if (ch != '\r') {
            throw ProtocolException("Chunk Trailer: Missing CR!");
        }

        if ((ch = Getc()) != '\n') {
            throw ProtocolException("Chunk Trailer: Missing LF!");
        }

        // Peek
        ch = Getc();
        if ((ch != ' ') && (ch != '\t')) {
            Ungetc();
            return value;
        }

        value += ' ';
    }
}


void DataReaderStream::SetEof() {
    RESTC_CPP_LOG_TRACE << "Reached EOF";
    eof_ = true;
}

} // namespace
