// Media Player Classic PS5: files on Windows shares (mpc_smb.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_smb.h"
#include "mpc_dialogs.h"

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

#include <algorithm>
#include <cstring>
#include <mutex>

namespace mpc
{
namespace smb
{
namespace
{
struct Url
{
	std::string user, password, domain, server, share, path; // path without the leading /
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
	const std::string url = WithStoredPassword(in);
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
	}
	u.server = auth_host;
	const size_t s2 = path.find('/');
	u.share = Decode(path.substr(0, s2));
	u.path = s2 == std::string::npos ? "" : Decode(path.substr(s2 + 1));
	while (!u.path.empty() && u.path.back() == '/')
		u.path.pop_back();
	return !u.server.empty();
}

smb2_context* Connect(const Url& u, const std::string& share, std::string* error)
{
	smb2_context* c = smb2_init_context();
	if (!c)
	{
		if (error)
			*error = T("Out of memory");
		return nullptr;
	}
	smb2_set_timeout(c, 15);
	smb2_set_security_mode(c, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_user(c, u.user.empty() ? "guest" : u.user.c_str());
	smb2_set_password(c, u.password.c_str());
	if (!u.domain.empty())
		smb2_set_domain(c, u.domain.c_str());
	if (smb2_connect_share(c, u.server.c_str(), share.c_str(), u.user.empty() ? "guest" : u.user.c_str()) < 0)
	{
		if (error)
			*error = Fmt(T("Can't connect to \\\\%s\\%s: %s"), u.server.c_str(), share.c_str(), smb2_get_error(c));
		OrbisLog("[smb] connect %s/%s: %s", u.server.c_str(), share.c_str(), smb2_get_error(c));
		smb2_destroy_context(c);
		return nullptr;
	}
	return c;
}

struct File
{
	smb2_context* ctx = nullptr;
	smb2fh* fh = nullptr;
	uint64_t pos = 0, size = 0;
	uint32_t max_read = 65536;
};

int ReadPacket(void* opaque, uint8_t* buf, int size)
{
	File* f = static_cast<File*>(opaque);
	if (f->pos >= f->size)
		return AVERROR_EOF;
	const uint32_t n = uint32_t(std::min<uint64_t>({uint64_t(size), f->size - f->pos, f->max_read}));
	const int r = smb2_pread(f->ctx, f->fh, buf, n, f->pos);
	if (r < 0)
	{
		OrbisLog("[smb] read at %llu: %s", (unsigned long long)f->pos, smb2_get_error(f->ctx));
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

File* OpenFile(const std::string& url, std::string* error)
{
	Url u;
	if (!Parse(url, u) || u.share.empty() || u.path.empty())
	{
		if (error)
			*error = Fmt(T("Not a file on a share: %s"), HidePassword(url).c_str());
		return nullptr;
	}
	smb2_context* c = Connect(u, u.share, error);
	if (!c)
		return nullptr;
	smb2fh* fh = smb2_open(c, u.path.c_str(), O_RDONLY);
	if (!fh)
	{
		if (error)
			*error = Fmt(T("Can't open %s: %s"), u.path.c_str(), smb2_get_error(c));
		smb2_disconnect_share(c);
		smb2_destroy_context(c);
		return nullptr;
	}
	// the size: the open's end-of-file (no round trip), else a query
	uint64_t size = 0;
	const int64_t end = smb2_lseek(c, fh, 0, SEEK_END, nullptr);
	if (end > 0)
		size = uint64_t(end);
	else
	{
		smb2_stat_64 st = {};
		if (smb2_fstat(c, fh, &st) == 0)
			size = st.smb2_size;
		else
			OrbisLog("[smb] fstat %s: %s", u.path.c_str(), smb2_get_error(c));
	}
	smb2_lseek(c, fh, 0, SEEK_SET, nullptr);
	File* f = new File;
	f->ctx = c;
	f->fh = fh;
	f->size = size;
	f->max_read = std::max<uint32_t>(65536, std::min<uint32_t>(smb2_get_max_read_size(c), 8u << 20));
	return f;
}

void CloseFile(File* f)
{
	if (!f)
		return;
	if (f->fh)
		smb2_close(f->ctx, f->fh);
	smb2_disconnect_share(f->ctx);
	smb2_destroy_context(f->ctx);
	delete f;
}
} // namespace

AVIOContext* OpenAvio(const std::string& url, void** owner, std::string* error)
{
	File* f = OpenFile(url, error);
	if (!f)
		return nullptr;
	const int bufsize = 1 << 20;
	uint8_t* buf = static_cast<uint8_t*>(av_malloc(bufsize));
	AVIOContext* io = avio_alloc_context(buf, bufsize, 0, f, ReadPacket, nullptr, SeekPacket);
	if (!io)
	{
		av_free(buf);
		CloseFile(f);
		return nullptr;
	}
	io->seekable = AVIO_SEEKABLE_NORMAL;
	*owner = f;
	OrbisLog("[smb] opened %s (%llu bytes, reads of %u)", url.c_str(), (unsigned long long)f->size, f->max_read);
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

bool ReadFile(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes)
{
	File* f = OpenFile(url, nullptr);
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
	smb2_context* c = Connect(u, u.share, nullptr);
	if (!c)
		return false;
	smb2_stat_64 st = {};
	const bool ok = smb2_stat(c, u.path.c_str(), &st) == 0;
	smb2_disconnect_share(c);
	smb2_destroy_context(c);
	return ok;
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
	smb2_context* c = Connect(u, u.share, error);
	if (!c)
		return false;
	smb2dir* d = smb2_opendir(c, u.path.c_str());
	if (!d)
	{
		if (error)
			*error = Fmt(T("Can't list the folder: %s"), smb2_get_error(c));
		smb2_disconnect_share(c);
		smb2_destroy_context(c);
		return false;
	}
	while (smb2dirent* e = smb2_readdir(c, d))
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
	smb2_closedir(c, d);
	smb2_disconnect_share(c);
	smb2_destroy_context(c);
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
	smb2_context* c = Connect(u, "IPC$", error);
	if (!c)
		return false;
	smb2_share_enum_reply* rep = smb2_share_enum_sync(c, SMB2_SHARE_INFO_1);
	bool ok = rep != nullptr;
	if (rep)
	{
		for (uint32_t i = 0; i < rep->entries_read; i++)
		{
			const smb2_share_info_1& s = rep->share_info.info_1[i];
			if ((s.type & 3) == SMB2_SHARE_TYPE_DISKTREE && !(s.type & SMB2_SHARE_TYPE_HIDDEN) && s.netname)
				out.push_back(s.netname);
		}
		smb2_free_data(c, rep);
	}
	else if (error)
		*error = Fmt(T("Can't list the shares: %s"), smb2_get_error(c));
	smb2_disconnect_share(c);
	smb2_destroy_context(c);
	return ok;
}
} // namespace smb
} // namespace mpc
