#include <HalonMTA.h>
#include <string>
#include <thread>
#include <queue>
#include <list>
#include <mutex>
#include <condition_variable>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <mkdirp.hpp>
#include <zlib.h>

struct halon {
	HalonDeliverContext *hdc;
	FILE *fp = nullptr;
	int outfd = -1;
	bool fsync;
	int compresslevel;
	std::string filename;
	std::string filename2;
};

std::list<std::thread> threads;
bool cquit = false;
std::mutex mcopy;
std::condition_variable cvcopy;
std::queue<halon*> qcopy;

struct
{
	bool fsync = true;
	bool compress = false;
	int compresslevel = 6;
	std::string basedir;
	std::string tmpbasedir;
} globalSettings;

// Deflates the message into the temporary file with a gzip header. On
// failure it sets the error reason and returns false.
bool gzcopy(halon *a, char *buf, size_t buflen)
{
	unsigned char out[16384];
	z_stream zs = {};
	if (deflateInit2(&zs, a->compresslevel, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
	{
		HalonMTA_deliver_setinfo(a->hdc, HALONMTA_ERROR_REASON, "Could not initialize gzip", 0);
		return false;
	}
	int flush = Z_NO_FLUSH;
	do
	{
		size_t r = fread(buf, 1, buflen, a->fp);
		if (r < buflen && ferror(a->fp))
		{
			HalonMTA_deliver_setinfo(a->hdc, HALONMTA_ERROR_REASON,
					("Could not read the message (" + a->filename + ")").c_str(), 0);
			deflateEnd(&zs);
			return false;
		}
		flush = r < buflen ? Z_FINISH : Z_NO_FLUSH;
		zs.next_in = (Bytef*)buf;
		zs.avail_in = (uInt)r;
		do
		{
			zs.next_out = out;
			zs.avail_out = sizeof(out);
			deflate(&zs, flush);
			size_t n = sizeof(out) - zs.avail_out;
			if (n && write(a->outfd, out, n) != (ssize_t)n)
			{
				HalonMTA_deliver_setinfo(a->hdc, HALONMTA_ERROR_REASON,
						("Could not write (" + a->filename + "): " + std::string(strerror(errno))).c_str(), 0);
				deflateEnd(&zs);
				return false;
			}
		} while (zs.avail_out == 0);
	} while (flush != Z_FINISH);
	deflateEnd(&zs);
	return true;
}

// Reads "none" or "gzip". Returns false for any other value.
bool parse_compress(const char *value, bool &compress)
{
	std::string v(value);
	if (v == "gzip")
		compress = true;
	else if (v == "none")
		compress = false;
	else
		return false;
	return true;
}

void fcopy()
{
	pthread_setname_np(pthread_self(), "p/file/copy");
	char buf[8192];
	while (true)
	{
		std::unique_lock<std::mutex> ul(mcopy);
		cvcopy.wait(ul, [&]() { return cquit || !qcopy.empty(); });
		if (cquit)
			break;
		auto a = qcopy.front();
		qcopy.pop();
		ul.unlock();

		if (!a->compresslevel)
		{
			while (true)
			{
				size_t r = fread(buf, 1, sizeof(buf), a->fp);
				if (r == 0)
					break;
				if (write(a->outfd, buf, r) != (ssize_t)r)
				{
					HalonMTA_deliver_setinfo(a->hdc, HALONMTA_ERROR_REASON,
							("Could not write (" + a->filename + "): " + std::string(strerror(errno))).c_str(), 0);
					unlink(a->filename.c_str());
					goto done;
				}
			}
		}
		else if (!gzcopy(a, buf, sizeof(buf)))
		{
			unlink(a->filename.c_str());
			goto done;
		}

		if (a->fsync && fsync(a->outfd) != 0)
		{
			HalonMTA_deliver_setinfo(a->hdc, HALONMTA_ERROR_REASON,
					("Could not fsync (" + a->filename + "): " + std::string(strerror(errno))).c_str(), 0);
			unlink(a->filename.c_str());
			goto done;
		}

		if (rename(a->filename.c_str(), a->filename2.c_str()) != 0)
		{
			if (errno == ENOENT)
			{
				auto slash = a->filename2.find_last_of('/');
				if (slash == std::string::npos)
				{
					errno = EINVAL;
					goto bad;
				}
				if (!mkdirp(a->filename2.substr(0, slash), 0777))
					goto bad;
				if (rename(a->filename.c_str(), a->filename2.c_str()) == 0)
					goto good;
			}
bad:
			HalonMTA_deliver_setinfo(a->hdc, HALONMTA_ERROR_REASON,
					("Could not rename (" + a->filename + ", " + a->filename2 + "): " + std::string(strerror(errno))).c_str(), 0);
			unlink(a->filename.c_str());
			goto done;
		}
good:
		{
			short int status = 250;
			HalonMTA_deliver_setinfo(a->hdc, HALONMTA_RESULT_CODE, &status, 0);
		}
		HalonMTA_deliver_setinfo(a->hdc, HALONMTA_RESULT_REASON, "FILE", 0);
done:
		close(a->outfd);
		HalonMTA_deliver_done(a->hdc);
		delete a;
	}
}

HALON_EXPORT
int Halon_version()
{
	return HALONMTA_PLUGIN_VERSION;
}

HALON_EXPORT
bool Halon_init(HalonInitContext *hic)
{
	HalonConfig* cfg;
	HalonMTA_init_getinfo(hic, HALONMTA_INIT_CONFIG, nullptr, 0, &cfg, nullptr);

	const char* path_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "path"), nullptr);
	if (path_)
		globalSettings.basedir = path_;

	const char* tmppath_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "tmppath"), nullptr);
	if (tmppath_)
		globalSettings.tmpbasedir = tmppath_;
	else
		globalSettings.tmpbasedir = globalSettings.basedir;

	const char* fsync_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "fsync"), nullptr);
	if (fsync_)
		globalSettings.fsync = std::string(fsync_) != "false";

	size_t threadcount = 1;
	const char* compress_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "compress"), nullptr);
	if (compress_ && !parse_compress(compress_, globalSettings.compress))
		return false;

	const char* compresslevel_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "compress_level"), nullptr);
	if (compresslevel_)
	{
		globalSettings.compresslevel = (int)strtol(compresslevel_, nullptr, 10);
		if (globalSettings.compresslevel < 1 || globalSettings.compresslevel > 9)
			return false;
	}

	const char* threads_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "threads"), nullptr);
	if (threads_)
		threadcount = strtoul(threads_, nullptr, 10);

	for (size_t i = 0; i < threadcount; ++i)
		threads.push_back(std::thread(fcopy));
	return true;
}

HALON_EXPORT
void Halon_cleanup()
{
	cquit = true;
	cvcopy.notify_all();
	for (auto & t : threads)
		t.join();
}

HALON_EXPORT
void Halon_deliver(HalonDeliverContext *hdc)
{
	if (globalSettings.basedir.empty())
	{
		HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON, "Path not configured", 0);
		HalonMTA_deliver_done(hdc);
		return;
	}

	FILE *fp = nullptr;
	if (!HalonMTA_deliver_getinfo(hdc, HALONMTA_INFO_FILE, nullptr, 0, (void*)&fp, nullptr))
	{
		HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON, "No file (internal error)", 0);
		HalonMTA_deliver_done(hdc);
		return;
	}

	HalonQueueMessage* hqm;
	const char* transactionid;
	size_t queueid;
	if (!HalonMTA_deliver_getinfo(hdc, HALONMTA_INFO_MESSAGE, nullptr, 0, (void*)&hqm, nullptr) ||
		!HalonMTA_message_getinfo(hqm, HALONMTA_MESSAGE_TRANSACTIONID, nullptr, 0, &transactionid, nullptr) ||
		!HalonMTA_message_getinfo(hqm, HALONMTA_MESSAGE_QUEUEID, nullptr, 0, &queueid, nullptr))
	{
		HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON, "No message (internal error)", 0);
		HalonMTA_deliver_done(hdc);
		return;
	}

	bool fsync_ = globalSettings.fsync;
	bool compress_ = globalSettings.compress;
	double compresslevel_ = globalSettings.compresslevel;
	const char *filename = nullptr;

	const HalonHSLValue *arguments = nullptr;
	if (HalonMTA_deliver_getinfo(hdc, HALONMTA_INFO_ARGUMENTS, nullptr, 0, &arguments, nullptr))
	{
		const HalonHSLValue *hv_fsync = HalonMTA_hsl_value_array_find(arguments, "fsync");
		if (hv_fsync && !HalonMTA_hsl_value_get(hv_fsync, HALONMTA_HSL_TYPE_BOOLEAN, &fsync_, nullptr))
		{
			HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON, "Bad fsync value", 0);
			HalonMTA_deliver_done(hdc);
			return;
		}

		const HalonHSLValue *hv_compress = HalonMTA_hsl_value_array_find(arguments, "compress");
		const char *compress = nullptr;
		if (hv_compress && (!HalonMTA_hsl_value_get(hv_compress, HALONMTA_HSL_TYPE_STRING, &compress, nullptr) ||
			!parse_compress(compress, compress_)))
		{
			HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON, "Bad compress value", 0);
			HalonMTA_deliver_done(hdc);
			return;
		}

		const HalonHSLValue *hv_compresslevel = HalonMTA_hsl_value_array_find(arguments, "compress_level");
		if (hv_compresslevel && (!HalonMTA_hsl_value_get(hv_compresslevel, HALONMTA_HSL_TYPE_NUMBER, &compresslevel_, nullptr) ||
			compresslevel_ < 1 || compresslevel_ > 9))
		{
			HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON, "Bad compress_level value", 0);
			HalonMTA_deliver_done(hdc);
			return;
		}

		const HalonHSLValue *hv_filename = HalonMTA_hsl_value_array_find(arguments, "filename");
		if (hv_filename && !HalonMTA_hsl_value_get(hv_filename, HALONMTA_HSL_TYPE_STRING, &filename, nullptr))
		{
			HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON, "Bad filename value", 0);
			HalonMTA_deliver_done(hdc);
			return;
		}
	}

	auto h = new halon;
	h->hdc = hdc;
	h->fp = fp;
	h->fsync = fsync_;
	h->compresslevel = compress_ ? (int)compresslevel_ : 0;
	h->filename = globalSettings.tmpbasedir + "/" + std::string(transactionid) + "_" + std::to_string(queueid) + ".tmp";
	if (filename)
		h->filename2 = globalSettings.basedir + "/" + filename;
	else
		h->filename2 = globalSettings.basedir + "/" + std::string(transactionid) + "_" + std::to_string(queueid) + (compress_ ? ".eml.gz" : ".eml");
	h->outfd = open(h->filename.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);

	if (h->outfd < 0)
	{
		HalonMTA_deliver_setinfo(hdc, HALONMTA_ERROR_REASON,
			("Could not open (" + h->filename + "): " + std::string(strerror(errno))).c_str(), 0);
		HalonMTA_deliver_done(hdc);
		delete h;
		return;
	}

	mcopy.lock();
	qcopy.push(h);
	mcopy.unlock();
	cvcopy.notify_one();
}