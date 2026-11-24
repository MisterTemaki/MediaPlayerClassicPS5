// Media Player Classic PS5: files on Windows shares (mpc_smb.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_smb.h"
#include "mpc_dialogs.h"

#include "mpc_credentials.h"
#include "mpc_settings.h"

#include "mpc_i18n.h"

#include "OrbisPaths.h"

#include <fcntl.h>

extern "C" {
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-share-enum.h>
#include <smb2/libsmb2-raw.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>

namespace mpc
{
namespace smb
{
namespace
{
struct Url
{
	std::string user, password, domain, server, share, path; // path without the leading /
	// 1.6.4 (audit C1): the password wiped when the address is done with, as the credential store wipes its copies
	~Url() { credentials::Wipe(password); }
};

std::string Decode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
			isxdigit(static_cast<unsigned char>(s[i + 2])))
		{
			out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		}
		else
			out += s[i];
	}
	return out;
}

bool Parse(const std::string& in, Url& u)
{
	if (in.compare(0, 6, "smb://") != 0)
		return false;
	// smb://user@server/... typed without the password: the one kept for that user and server, if any
	std::string url = WithStoredPassword(in);
	std::string rest = url.substr(6);
	const size_t slash = rest.find('/');
	std::string auth_host = rest.substr(0, slash);
	std::string path = slash == std::string::npos ? "" : rest.substr(slash + 1);
	const size_t at = auth_host.rfind('@');
	if (at != std::string::npos)
	{
		std::string auth = auth_host.substr(0, at);
		auth_host = auth_host.substr(at + 1);
		const size_t semi = auth.find(';');
		if (semi != std::string::npos)
		{
			u.domain = Decode(auth.substr(0, semi));
			auth = auth.substr(semi + 1);
		}
		const size_t colon = auth.find(':');
		u.user = Decode(auth.substr(0, colon));
		if (colon != std::string::npos)
			u.password = Decode(auth.substr(colon + 1));
		credentials::Wipe(auth);
	}
	u.server = auth_host;
	// (audit C1) the copies that held the password
	credentials::Wipe(rest);
	credentials::Wipe(url);
	const size_t s2 = path.find('/');
	u.share = Decode(path.substr(0, s2));
	u.path = s2 == std::string::npos ? "" : Decode(path.substr(s2 + 1));
	while (!u.path.empty() && u.path.back() == '/')
		u.path.pop_back();
	return !u.server.empty();
}

// ---- requests -----------------------------------------------------------------------------------------------
// (1.7.1, audits B1, C1, D1, D2) libsmb2's sync calls wait in a loop of their own (a poll of a second at a time, up to
// the 15 s timeout of each request) that nothing outside can stop: a share that stopped answering held Close, the next
// file and the file browser for that long, each time. Every request now goes through Run: libsmb2's async call and the
// same loop, polled every 0.1 s, which gives up at once when the cancel flag is set (CancelScope) or the deadline
// passes. A request given up stays with libsmb2: its record is freed by its callback when that comes (from the
// context's destroy: a connection with a request given up is "lost", never used again, only destroyed).

thread_local const std::atomic<bool>* t_cancel = nullptr;
thread_local double t_deadline = 0; // (steady seconds; 0: none) ReadFile's limit

double SteadyNow()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// a connection: the context, the cancel flag of whoever made it (a file's reads use it from the demuxer's thread)
struct Conn
{
	smb2_context* ctx = nullptr;
	const std::atomic<bool>* cancel = nullptr;
	double deadline = 0;
	bool lost = false;	 // a request given up: only destroyed
	bool cancelled = false; // (why: the flag, not the server)
};

struct Call
{
	bool finished = false;
	bool abandoned = false; // the waiter gave up: the callback frees the record
	int status = 0;
	void* data = nullptr;
	smb2_stat_64 st = {}; // (stat replies are written here: alive until the callback)
	enum Kind
	{
		kPlain,
		kDir,
		kShares
	} kind = kPlain;
};

void Done(smb2_context* ctx, int status, void* data, void* priv)
{
	Call* c = static_cast<Call*>(priv);
	if (c->abandoned)
	{
		// nobody waits: what the reply carried goes too (in practice the destroy's call carries nothing)
		if (data && c->kind == Call::kDir)
			smb2_closedir(ctx, static_cast<smb2dir*>(data));
		else if (data && c->kind == Call::kShares)
			smb2_free_data(ctx, data);
		delete c;
		return;
	}
	c->finished = true;
	c->status = status;
	c->data = data;
}

constexpr int kCancelled = -ECANCELED, kTimedOut = -ETIMEDOUT;

// the request's status (libsmb2's: >= 0, or -errno), or kCancelled / kTimedOut / -EIO when given up (k.lost set).
// start(call) queues the request with Done and the call; it returns < 0 when nothing was queued.
template <typename Start>
int Run(Conn& k, Call::Kind kind, Start start, Call** result, int limit_s = 30)
{
	*result = nullptr;
	if (k.lost)
		return -EIO;
	Call* c = new (std::nothrow) Call;
	if (!c)
		return -ENOMEM;
	c->kind = kind;
	const int q = start(c);
	if (q < 0)
	{
		delete c; // (nothing queued: no callback will come)
		return q;
	}
	const double t0 = SteadyNow();
	while (!c->finished)
	{
		const double now = SteadyNow();
		int why = 0;
		if (k.cancel && k.cancel->load())
			why = kCancelled;
		else if ((k.deadline > 0 && now > k.deadline) || now - t0 > limit_s)
			why = kTimedOut; // (a backstop: libsmb2 times each request out at 15 s itself)
		if (why)
		{
			c->abandoned = true;
			k.lost = true;
			k.cancelled = why == kCancelled;
			return why;
		}
		pollfd p = {};
		p.fd = smb2_get_fd(k.ctx);
		p.events = short(smb2_which_events(k.ctx));
		const int r = poll(&p, p.fd >= 0 ? 1 : 0, 100);
		if (r < 0 && errno != EINTR)
		{
			c->abandoned = true;
			k.lost = true;
			return -EIO;
		}
		// (revents 0 too: libsmb2 times its requests out in smb2_service)
		if (p.fd >= 0 && smb2_service(k.ctx, r > 0 ? p.revents : 0) < 0)
		{
			k.lost = true; // (the context can't be used again: libsmb2's rule)
			if (!c->finished)
			{
				c->abandoned = true;
				return -EIO;
			}
			break; // (the callback came with the failure)
		}
	}
	*result = c;
	return c->status;
}

// a request whose result nobody needs: its record freed by its callback
template <typename Start>
void Forget(Conn& k, Start start)
{
	Call* c = new (std::nothrow) Call;
	if (!c)
		return;
	c->abandoned = true;
	if (start(c) < 0)
		delete c;
}

const char* ErrorText(const Conn& k, int rc)
{
	if (rc == kCancelled)
		return T("cancelled");
	if (rc == kTimedOut)
		return T("no answer in time");
	return smb2_get_error(k.ctx);
}

void Destroy(Conn* k)
{
	if (!k)
		return;
	if (k->ctx)
		smb2_destroy_context(k->ctx); // (calls the callbacks of requests still queued)
	delete k;
}

// the goodbye to the server (skipped when the connection is lost or its owner gave up), then the context's end
void Disconnect(Conn* k)
{
	if (!k)
		return;
	if (!k->lost && !(k->cancel && k->cancel->load()))
	{
		Call* c = nullptr;
		Run(*k, Call::kPlain, [&](Call* x) { return smb2_disconnect_share_async(k->ctx, Done, x); }, &c, 15);
		delete c;
	}
	Destroy(k);
}

Conn* Connect(const Url& u, const std::string& share, std::string* error)
{
	smb2_context* ctx = smb2_init_context();
	Conn* k = ctx ? new (std::nothrow) Conn : nullptr;
	if (!k)
	{
		if (ctx)
			smb2_destroy_context(ctx);
		if (error)
			*error = T("Out of memory");
		return nullptr;
	}
	k->ctx = ctx;
	k->cancel = t_cancel;
	k->deadline = t_deadline;
	smb2_set_timeout(ctx, 15);
	smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_user(ctx, u.user.empty() ? "guest" : u.user.c_str());
	smb2_set_password(ctx, u.password.c_str());
	if (!u.domain.empty())
		smb2_set_domain(ctx, u.domain.c_str());
	Call* c = nullptr;
	// (20 s: the TCP connection, the negotiation, the session and the share; 1.7.0 waited 15 s for the TCP part alone,
	// then up to 15 s for each step)
	const int rc = Run(*k, Call::kPlain,
		[&](Call* x) {
			return smb2_connect_share_async(ctx, u.server.c_str(), share.c_str(), u.user.empty() ? "guest" : u.user.c_str(),
				Done, x);
		},
		&c, 20);
	delete c;
	if (rc < 0)
	{
		if (error)
			*error = Fmt(T("Can't connect to \\\\%s\\%s: %s"), u.server.c_str(), share.c_str(), ErrorText(*k, rc));
		OrbisLog("[smb] connect %s/%s: %s", u.server.c_str(), share.c_str(),
			rc == kCancelled ? "cancelled" : rc == kTimedOut ? "no answer in time" : smb2_get_error(ctx));
		Destroy(k);
		return nullptr;
	}
	return k;
}

struct File
{
	Conn* conn = nullptr;
	smb2fh* fh = nullptr;
	uint64_t pos = 0, size = 0;
	uint32_t max_read = 65536;
};

int ReadPacket(void* opaque, uint8_t* buf, int size)
{
	File* f = static_cast<File*>(opaque);
	if (f->conn->cancel && f->conn->cancel->load())
		return AVERROR_EXIT; // (the player closing: FFmpeg's "stop", not an error to report)
	if (f->conn->lost)
		return AVERROR(EIO);
	if (f->pos >= f->size)
		return AVERROR_EOF;
	const uint32_t n = uint32_t(std::min<uint64_t>({uint64_t(size), f->size - f->pos, f->max_read}));
	Call* c = nullptr;
	const int r = Run(*f->conn, Call::kPlain,
		[&](Call* x) { return smb2_pread_async(f->conn->ctx, f->fh, buf, n, f->pos, Done, x); }, &c);
	delete c;
	if (r == kCancelled)
		return AVERROR_EXIT;
	if (r < 0)
	{
		OrbisLog("[smb] read at %llu: %s", (unsigned long long)f->pos, r == kTimedOut ? "no answer in time" : smb2_get_error(f->conn->ctx));
		return AVERROR(EIO);
	}
	if (r == 0)
		return AVERROR_EOF;
	f->pos += uint64_t(r);
	return r;
}

int64_t SeekPacket(void* opaque, int64_t offset, int whence)
{
	File* f = static_cast<File*>(opaque);
	if (whence == AVSEEK_SIZE)
		return int64_t(f->size);
	whence &= ~AVSEEK_FORCE;
	int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? int64_t(f->pos) + offset : int64_t(f->size) + offset;
	if (p < 0)
		return AVERROR(EINVAL);
	f->pos = uint64_t(p);
	return p;
}

void CloseFile(File* f)
{
	if (!f)
		return;
	if (f->fh)
	{
		Conn& k = *f->conn;
		if (!k.lost && !(k.cancel && k.cancel->load()))
		{
			Call* c = nullptr;
			Run(k, Call::kPlain, [&](Call* x) { return smb2_close_async(k.ctx, f->fh, Done, x); }, &c, 15);
			delete c;
		}
		else
			// (lost, or its owner gave up: no wait; the destroy below ends the request, and libsmb2's close
			// callback frees the handle then)
			Forget(k, [&](Call* x) { return smb2_close_async(k.ctx, f->fh, Done, x); });
	}
	Disconnect(f->conn);
	delete f;
}

File* OpenFile(const std::string& url, std::string* error)
{
	Url u;
	if (!Parse(url, u) || u.share.empty() || u.path.empty())
	{
		if (error)
			*error = Fmt(T("Not a file on a share: %s"), HidePassword(url).c_str());
		return nullptr;
	}
	Conn* k = Connect(u, u.share, error);
	if (!k)
		return nullptr;
	Call* c = nullptr;
	const int rc = Run(*k, Call::kPlain,
		[&](Call* x) { return smb2_open_async(k->ctx, u.path.c_str(), O_RDONLY, Done, x); }, &c);
	smb2fh* fh = rc >= 0 && c ? static_cast<smb2fh*>(c->data) : nullptr;
	delete c;
	if (!fh)
	{
		if (error)
			*error = Fmt(T("Can't open %s: %s"), u.path.c_str(), rc < 0 ? ErrorText(*k, rc) : smb2_get_error(k->ctx));
		Disconnect(k);
		return nullptr;
	}
	File* f = new (std::nothrow) File;
	if (!f)
	{
		Forget(*k, [&](Call* x) { return smb2_close_async(k->ctx, fh, Done, x); });
		Destroy(k);
		if (error)
			*error = T("Out of memory");
		return nullptr;
	}
	f->conn = k;
	f->fh = fh;
	// the size: the open's end-of-file (no round trip), else a query
	const int64_t end = smb2_lseek(k->ctx, fh, 0, SEEK_END, nullptr);
	if (end > 0)
		f->size = uint64_t(end);
	else
	{
		const int s = Run(*k, Call::kPlain, [&](Call* x) { return smb2_fstat_async(k->ctx, fh, &x->st, Done, x); }, &c);
		if (s == 0 && c)
			f->size = c->st.smb2_size;
		else
			OrbisLog("[smb] fstat %s: %s", u.path.c_str(), s < 0 ? ErrorText(*k, s) : smb2_get_error(k->ctx));
		delete c;
		if (k->lost)
		{
			if (error)
				*error = Fmt(T("Can't open %s: %s"), u.path.c_str(), ErrorText(*k, s));
			CloseFile(f);
			return nullptr;
		}
	}
	smb2_lseek(k->ctx, fh, 0, SEEK_SET, nullptr);
	f->max_read = std::max<uint32_t>(65536, std::min<uint32_t>(smb2_get_max_read_size(k->ctx), 8u << 20));
	return f;
}
} // namespace

std::string FileNameOf(const std::string& url)
{
	std::string u = url;
	while (u.size() > 6 && u.back() == '/')
		u.pop_back();
	const size_t slash = u.find_last_of('/');
	return Decode(slash == std::string::npos ? u : u.substr(slash + 1));
}

std::string EscapeName(const std::string& name)
{
	std::string out;
	for (char c : name)
		out += c == '%' ? std::string("%25") : std::string(1, c);
	return out;
}

CancelScope::CancelScope(const std::atomic<bool>* flag) : prev_(t_cancel)
{
	t_cancel = flag;
}

CancelScope::~CancelScope()
{
	t_cancel = prev_;
}

AVIOContext* OpenAvio(const std::string& url, void** owner, std::string* error)
{
	File* f = OpenFile(url, error);
	if (!f)
		return nullptr;
	const int bufsize = 1 << 20;
	uint8_t* buf = static_cast<uint8_t*>(av_malloc(bufsize));
	AVIOContext* io = buf ? avio_alloc_context(buf, bufsize, 0, f, ReadPacket, nullptr, SeekPacket) : nullptr;
	if (!io)
	{
		av_free(buf);
		CloseFile(f);
		return nullptr;
	}
	io->seekable = AVIO_SEEKABLE_NORMAL;
	*owner = f;
	OrbisLog("[smb] opened %s (%llu bytes, reads of %u)", HidePassword(url).c_str(), (unsigned long long)f->size, f->max_read);
	return io;
}

void CloseAvio(AVIOContext* io, void* owner)
{
	if (io)
	{
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
	CloseFile(static_cast<File*>(owner));
}

bool ReadFile(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes, double max_seconds)
{
	const double prev = t_deadline;
	if (max_seconds > 0)
		t_deadline = SteadyNow() + max_seconds;
	File* f = OpenFile(url, nullptr);
	t_deadline = prev;
	if (!f)
		return false;
	bool ok = f->size <= max_bytes;
	if (ok)
	{
		out.resize(size_t(f->size));
		size_t got = 0;
		while (got < out.size())
		{
			const int r = ReadPacket(f, out.data() + got, int(std::min<size_t>(out.size() - got, 1 << 20)));
			if (r <= 0)
				break;
			got += size_t(r);
		}
		// (1.7.1) a read cut short (the deadline, Close, the server gone) isn't the file
		ok = got == out.size();
		out.resize(got);
	}
	CloseFile(f);
	return ok;
}

bool Exists(const std::string& url)
{
	Url u;
	if (!Parse(url, u) || u.share.empty())
		return false;
	Conn* k = Connect(u, u.share, nullptr);
	if (!k)
		return false;
	Call* c = nullptr;
	const int rc = Run(*k, Call::kPlain, [&](Call* x) { return smb2_stat_async(k->ctx, u.path.c_str(), &x->st, Done, x); }, &c);
	delete c;
	Disconnect(k);
	return rc == 0;
}

bool List(const std::string& url, std::vector<Entry>& out, std::string* error)
{
	Url u;
	if (!Parse(url, u) || u.share.empty())
	{
		if (error)
			*error = Fmt(T("No share in %s"), HidePassword(url).c_str());
		return false;
	}
	Conn* k = Connect(u, u.share, error);
	if (!k)
		return false;
	Call* c = nullptr;
	const int rc = Run(*k, Call::kDir, [&](Call* x) { return smb2_opendir_async(k->ctx, u.path.c_str(), Done, x); }, &c);
	smb2dir* d = rc >= 0 && c ? static_cast<smb2dir*>(c->data) : nullptr;
	delete c;
	if (!d)
	{
		if (error)
			*error = Fmt(T("Can't list the folder: %s"), rc < 0 ? ErrorText(*k, rc) : smb2_get_error(k->ctx));
		Disconnect(k);
		return false;
	}
	while (smb2dirent* e = smb2_readdir(k->ctx, d)) // (the listing is in memory: no requests)
	{
		if (!e->name || !strcmp(e->name, ".") || !strcmp(e->name, ".."))
			continue;
		Entry en;
		en.name = e->name;
		en.dir = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
		en.size = e->st.smb2_size;
		en.mtime = int64_t(e->st.smb2_mtime);
		out.push_back(en);
	}
	smb2_closedir(k->ctx, d);
	Disconnect(k);
	return true;
}

bool Shares(const std::string& url, std::vector<std::string>& out, std::string* error)
{
	Url u;
	if (!Parse(url, u))
	{
		if (error)
			*error = T("Not an smb:// address");
		return false;
	}
	Conn* k = Connect(u, "IPC$", error);
	if (!k)
		return false;
	Call* c = nullptr;
	const int rc = Run(*k, Call::kShares, [&](Call* x) { return smb2_share_enum_async(k->ctx, SMB2_SHARE_INFO_1, Done, x); }, &c);
	smb2_share_enum_reply* rep = c ? static_cast<smb2_share_enum_reply*>(c->data) : nullptr;
	delete c;
	const bool ok = rc == 0 && rep;
	if (ok)
	{
		for (uint32_t i = 0; i < rep->entries_read; i++)
		{
			const smb2_share_info_1& s = rep->share_info.info_1[i];
			if ((s.type & 3) == SMB2_SHARE_TYPE_DISKTREE && !(s.type & SMB2_SHARE_TYPE_HIDDEN) && s.netname)
				out.push_back(s.netname);
		}
	}
	else if (error)
		*error = Fmt(T("Can't list the shares: %s"), rc < 0 ? ErrorText(*k, rc) : rc > 0 ? Fmt("WERROR 0x%08x", unsigned(rc)).c_str() : smb2_get_error(k->ctx));
	if (rep)
		smb2_free_data(k->ctx, rep); // (a WERROR still comes with a reply)
	Disconnect(k);
	return ok;
}
} // namespace smb
} // namespace mpc
