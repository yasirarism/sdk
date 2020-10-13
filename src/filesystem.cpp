/**
 * @file filesystem.cpp
 * @brief Generic host filesystem access interfaces
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */
#include <cctype>

#include "mega/filesystem.h"
#include "mega/node.h"
#include "mega/megaclient.h"
#include "mega/logging.h"
#include "mega/mega_utf8proc.h"

namespace mega {

namespace detail {

template<typename CharT>
bool isEscape(UnicodeCodepointIterator<CharT> it);

template<typename CharT>
int decodeEscape(UnicodeCodepointIterator<CharT>& it)
{
    assert(isEscape(it));

    // Skip the leading %.
    (void)it.get();

    return hexval(it.get()) << 4 | hexval(it.get());
}

int identity(const int c)
{
    return c;
}

template<typename CharT>
bool isControlEscape(UnicodeCodepointIterator<CharT> it)
{
    if (isEscape(it))
    {
        const int32_t c = decodeEscape(it);

        return c < 0x20 || c == 0x7f;
    }

    return false;
}

template<typename CharT>
bool isEscape(UnicodeCodepointIterator<CharT> it)
{
    return it.get() == '%'
           && islchex(it.get())
           && islchex(it.get());
}

template<typename CharT, typename CharU, typename UnaryOperation>
int localCompare(UnicodeCodepointIterator<CharT> first1,
                 UnicodeCodepointIterator<CharU> first2,
                 UnaryOperation transform)
{
    while (!(first1.end() || first2.end()))
    {
        int c1;
        int c2;

        if (isEscape(first1))
        {
            c1 = decodeEscape(first1);
        }
        else
        {
            c1 = first1.get();
        }

        if (isEscape(first2))
        {
            c2 = decodeEscape(first2);
        }
        else
        {
            c2 = first2.get();
        }

        c1 = transform(c1);
        c2 = transform(c2);

        if (c1 != c2)
        {
            return c1 - c2;
        }
    }

    if (first1.end() && first2.end())
    {
        return 0;
    }

    if (first1.end())
    {
        return -1;
    }

    return 1;
}

template<typename CharT, typename CharU, typename UnaryOperation>
int remoteCompare(UnicodeCodepointIterator<CharT> first1,
                  UnicodeCodepointIterator<CharU> first2,
                  UnaryOperation transform)
{
    while (!(first1.end() || first2.end()))
    {
        int c1;
        int c2;

        if (isEscape(first1))
        {
            c1 = decodeEscape(first1);
        }
        else
        {
            c1 = first1.get();
        }

        if (isControlEscape(first2))
        {
            c2 = decodeEscape(first2);
        }
        else
        {
            c2 = first2.get();
        }

        c1 = transform(c1);
        c2 = transform(c2);

        if (c1 != c2)
        {
            return c1 - c2;
        }
    }

    if (first1.end() && first2.end())
    {
        return 0;
    }

    if (first1.end())
    {
        return -1;
    }

    return 1;
}

} // detail

static bool isCaseInsensitive(const FileSystemType type)
{
    return type == FS_EXFAT
           || type == FS_FAT32
           || type == FS_NTFS
           || type == FS_UNKNOWN;
}

NamePtrCmp::NamePtrCmp(FileSystemType type)
  : mType(type)
{
}

bool NamePtrCmp::operator()(const string* lhs, const string* rhs) const
{
    assert(lhs && rhs);

    auto transform =
      isCaseInsensitive(mType) ? Utils::toUpper
                               : detail::identity;

    return detail::remoteCompare(
             unicodeCodepointIterator(*lhs),
             unicodeCodepointIterator(*rhs),
             transform);
}

FileSystemAccess::FileSystemAccess()
    : waiter(NULL)
    , skip_errorreport(false)
    , transient_error(false)
    , notifyerr(false)
    , notifyfailed(false)
    , target_exists(false)
    , client(NULL)
{
}

void FileSystemAccess::captimestamp(m_time_t* t)
{
    // FIXME: remove upper bound before the year 2100 and upgrade server-side timestamps to BIGINT
    if (*t > (uint32_t)-1) *t = (uint32_t)-1;
    else if (*t < 0) *t = 0;
}

int FileSystemAccess::decodeEscape(const char *s) const
{
    if (!isEscape(s))
    {
        return -1;
    }

    return hexval(s[1]) << 4 | hexval(s[2]);
}

bool FileSystemAccess::isEscape(const char* s) const
{
    return *s == '%'
           && islchex(s[1])
           && islchex(s[2]);
}

const char *FileSystemAccess::fstypetostring(FileSystemType type) const
{
    switch (type)
    {
        case FS_NTFS:
            return "NTFS";
        case FS_EXFAT:
            return "EXFAT";
        case FS_FAT32:
            return "FAT32";
        case FS_EXT:
            return "EXT";
        case FS_HFS:
            return "HFS";
        case FS_APFS:
            return "APFS";
        case FS_FUSE:
            return "FUSE";
        case FS_SDCARDFS:
            return "SDCARDFS";
        case FS_F2FS:
            return "F2FS";
        case FS_XFS:
            return "XFS";
        case FS_UNKNOWN:    // fall through
            return "UNKNOWN FS";
    }

    return "UNKNOWN FS";
}

FileSystemType FileSystemAccess::getlocalfstype(const LocalPath& path) const
{
    // Not enough information to determine path.
    if (path.empty())
    {
        return FS_UNKNOWN;
    }

    FileSystemType type;

    // Try and get the type from the path we were given.
    if (getlocalfstype(path, type))
    {
        // Path exists.
        return type;
    }

    // Try and get the type based on our parent's path.
    LocalPath parentPath(path);

    // Remove trailing separator, if any.
    parentPath.trimNonDriveTrailingSeparator(localseparator);

    // Did the path consist solely of that separator?
    if (parentPath.empty())
    {
        return FS_UNKNOWN;
    }

    // Where does our name begin?
    auto index = parentPath.getLeafnameByteIndex(*this);

    // We have a parent.
    if (index)
    {
        // Remove the current leaf name.
        parentPath.truncate(index);

        // Try and get our parent's filesystem type.
        if (getlocalfstype(parentPath, type))
        {
            return type;
        }
    }

    return FS_UNKNOWN;
}

bool FileSystemAccess::islocalfscompatible(const int character, const FileSystemType type) const
{
    // NUL is always escaped.
    if (!character)
    {
        return false;
    }

    // Escape '%' if it is not encoding a control character.
    if (character == '%')
    {
        return false;
    }

    // Filesystem-specific policies.
    switch (type)
    {
    case FS_APFS:
    case FS_HFS:
        return character != ':' && character != '/';
    case FS_EXT:
    case FS_F2FS:
    case FS_XFS:
        return character != '/';
    case FS_EXFAT:
    case FS_FAT32:
    case FS_FUSE:
    case FS_NTFS:
    case FS_SDCARDFS:
    case FS_UNKNOWN:
    default:
        return !(std::iscntrl(character) || strchr("\\/:?\"<>|*", character));
    }
}

// replace characters that are not allowed in local fs names with a %xx escape sequence
void FileSystemAccess::escapefsincompatible(string* name, FileSystemType fileSystemType) const
{
    if (!name->compare(".."))
    {
        name->replace(0, 2, "%2e%2e");
        return;
    }

    if (!name->compare("."))
    {
        name->replace(0, 1, "%2e");
        return;
    }

    for (size_t i = 0; i < name->size(); )
    {
        int character;

        // Are we processing an escape sequence?
        if ((character = decodeEscape(&(*name)[i])) >= 0)
        {
            // Is it encoding a control character?
            if (std::iscntrl(character))
            {
                // Substitute the character in.
                // It'll be escaped again if necessary.
                name->replace(i, 3, 1, static_cast<char>(character));
            }
        }

        character = (*name)[i];
        auto seqsize = Utils::utf8SequenceSize(static_cast<char>(character));
        assert(seqsize);

        if (seqsize == 1 && !islocalfscompatible(character, fileSystemType))
        {
            char buffer[4];

            sprintf(buffer, "%%%02x", character);
            name->replace(i, 1, buffer);

            LOG_debug << "Escaped character for filesystem type "
                      << fstypetostring(fileSystemType)
                      << ": "
                      << buffer;

            seqsize = 3;
        }

        i += seqsize;
    }
}

void FileSystemAccess::unescapefsincompatible(string *name) const
{
    if (!name->compare("%2e%2e"))
    {
        name->replace(0, 6, "..");
        return;
    }

    if (!name->compare("%2e"))
    {
        name->replace(0, 3, ".");
        return;
    }

    for (size_t i = 0; i < name->size(); ++i)
    {
        // For convenience.
        const char* s = &(*name)[i];

        // Are we looking at a raw control character?
        int character = static_cast<uint8_t>(*s);

        // If so, escape it.
        if (std::iscntrl(character))
        {
            char buffer[4];

            sprintf(buffer, "%%%02x", character);
            name->replace(i, 1, buffer);

            // Skip over the encoded sequence.
            i += 2;
            continue;
        }

        // Are we processing an escape sequence?
        if ((character = decodeEscape(s)) < 0)
        {
            // Nope, continue.
            continue;
        }

        // Is the escape encoding a control character?
        if (std::iscntrl(character))
        {
            // Yup, skip the sequence.
            i += 2;
            continue;
        }

        // Substitute in the decoded character.
        name->replace(i, 3, 1, static_cast<char>(character));
    }
}

void FileSystemAccess::canonicalize(string* name) const
{
    for (size_t i = 0; i < name->size(); ++i)
    {
        int character = static_cast<uint8_t>((*name)[i]);

        // Have we encountered a raw control character?
        if (std::iscntrl(character))
        {
            // If so, escape it.
            char buffer[4];

            sprintf(buffer, "%%%02x", character);
            name->replace(i, 1, buffer);

            // Skip the newly inserted sequence.
            i += 2;
            continue;
        }

        // Have we encountered an escape sequence?
        if ((character = decodeEscape(&(*name)[i])) >= 0)
        {
            // Skip over the sequence.
            i += 2;
        }
    }
}

string FileSystemAccess::canonicalize(const string& name) const
{
    string result = name;

    canonicalize(&result);

    return result;
}

const char *FileSystemAccess::getPathSeparator()
{
#if defined (__linux__) || defined (__ANDROID__) || defined  (__APPLE__) || defined (USE_IOS)
    return "/";
#elif defined(_WIN32) || defined(WINDOWS_PHONE)
    return "\\";
#else
    // Default case
    LOG_warn << "No path separator found";
    return "\\/";
#endif
}

void FileSystemAccess::normalize(string* filename) const
{
    if (!filename) return;

    const char* cfilename = filename->c_str();
    size_t fnsize = filename->size();
    string result;

    for (size_t i = 0; i < fnsize; )
    {
        // allow NUL bytes between valid UTF-8 sequences
        if (!cfilename[i])
        {
            result.append("", 1);
            i++;
            continue;
        }

        const char* substring = cfilename + i;
        char* normalized = (char*)utf8proc_NFC((uint8_t*)substring);

        if (!normalized)
        {
            filename->clear();
            return;
        }

        result.append(normalized);
        free(normalized);

        i += strlen(substring);
    }

    *filename = std::move(result);
}

std::unique_ptr<LocalPath> FileSystemAccess::fsShortname(LocalPath& localname)
{
    LocalPath s;
    if (getsname(localname, s))
    {
        return ::mega::make_unique<LocalPath>(std::move(s));
    }
    return nullptr;
}

// default DirNotify: no notification available
DirNotify::DirNotify(const LocalPath& clocalbasepath, const LocalPath& cignore)
{
    localbasepath = clocalbasepath;
    ignore = cignore;

    mFailed = 1;
    mFailReason = "Not initialized";
    mErrorCount = 0;
    sync = NULL;
}


void DirNotify::setFailed(int errCode, const string& reason)
{
    std::lock_guard<std::mutex> g(mMutex);
    mFailed = errCode;
    mFailReason = reason;
}

int DirNotify::getFailed(string& reason)
{
    if (mFailed)
    {
        reason = mFailReason;
    }
    return mFailed;
}


// notify base LocalNode + relative path/filename
void DirNotify::notify(NotificationDeque& q, LocalNode* l, LocalPath&& path, bool immediate)
{
    // We may be executing on a thread here so we can't access the LocalNode data structures.  Queue everything, and
    // filter when the notifications are processed.  Also, queueing it here is faster than logging the decision anyway.

    Notification n;
    n.timestamp = immediate ? 0 : Waiter::ds;
    n.localnode = l;
    n.path = std::move(path);
    q.pushBack(std::move(n));
}

// default: no fingerprint
fsfp_t DirNotify::fsfingerprint() const
{
    return 0;
}

bool DirNotify::fsstableids() const
{
    return true;
}

DirNotify* FileSystemAccess::newdirnotify(LocalPath& localpath, LocalPath& ignore, Waiter*)
{
    return new DirNotify(localpath, ignore);
}

FileAccess::FileAccess(Waiter *waiter)
{
    this->waiter = waiter;
    this->isAsyncOpened = false;
    this->numAsyncReads = 0;
}

FileAccess::~FileAccess()
{
    // All AsyncIOContext objects must be deleted before
    assert(!numAsyncReads && !isAsyncOpened);
}

// open file for reading
bool FileAccess::fopen(const LocalPath& name)
{
    updatelocalname(name, true);

    return sysstat(&mtime, &size);
}

bool FileAccess::isfolder(LocalPath& name)
{
    fopen(name);
    return (type == FOLDERNODE);
}

// check if size and mtime are unchanged, then open for reading
bool FileAccess::openf()
{
    if (nonblocking_localname.empty())
    {
        // file was not opened in nonblocking mode
        return true;
    }

    m_time_t curr_mtime;
    m_off_t curr_size;
    if (!sysstat(&curr_mtime, &curr_size))
    {
        LOG_warn << "Error opening sync file handle (sysstat) "
                 << curr_mtime << " - " << mtime
                 << curr_size  << " - " << size;
        return false;
    }

    if (curr_mtime != mtime || curr_size != size)
    {
        mtime = curr_mtime;
        size = curr_size;
        retry = false;
        return false;
    }

    return sysopen();
}

void FileAccess::closef()
{
    if (!nonblocking_localname.empty())
    {
        sysclose();
    }
}

void FileAccess::asyncopfinished(void *param)
{
    Waiter *waiter = (Waiter *)param;
    if (waiter)
    {
        waiter->notify();
    }
}

AsyncIOContext *FileAccess::asyncfopen(const LocalPath& f)
{
    updatelocalname(f, true);

    LOG_verbose << "Async open start";
    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::OPEN;
    context->access = AsyncIOContext::ACCESS_READ;
    context->openPath = f;
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->posOfBuffer = size;
    context->fa = this;

    context->failed = !sysstat(&mtime, &size);
    context->retry = this->retry;
    context->finished = true;
    context->userCallback(context->userData);
    return context;
}

bool FileAccess::asyncopenf()
{
    numAsyncReads++;
    if (nonblocking_localname.empty())
    {
        return true;
    }

    if (isAsyncOpened)
    {
        return true;
    }

    m_time_t curr_mtime = 0;
    m_off_t curr_size = 0;
    if (!sysstat(&curr_mtime, &curr_size))
    {
        LOG_warn << "Error opening async file handle (sysstat) "
                 << curr_mtime << " - " << mtime
                 << curr_size  << " - " << size;
        return false;
    }

    if (curr_mtime != mtime || curr_size != size)
    {
        mtime = curr_mtime;
        size = curr_size;
        retry = false;
        return false;
    }

    LOG_debug << "Opening async file handle for reading";
    bool result = sysopen(true);
    if (result)
    {
        isAsyncOpened = true;
    }
    else
    {
        LOG_warn << "Error opening async file handle (sysopen)";
    }
    return result;
}

void FileAccess::asyncclosef()
{
    numAsyncReads--;
    if (isAsyncOpened && !numAsyncReads)
    {
        LOG_debug << "Closing async file handle";
        isAsyncOpened = false;
        sysclose();
    }
}

AsyncIOContext *FileAccess::asyncfopen(const LocalPath& f, bool read, bool write, m_off_t pos)
{
    LOG_verbose << "Async open start";
    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::OPEN;
    context->access = AsyncIOContext::ACCESS_NONE
            | (read ? AsyncIOContext::ACCESS_READ : 0)
            | (write ? AsyncIOContext::ACCESS_WRITE : 0);

    context->openPath = f;
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->posOfBuffer = pos;
    context->fa = this;

    asyncsysopen(context);
    return context;
}

void FileAccess::asyncsysopen(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::asyncfread(string *dst, unsigned len, unsigned pad, m_off_t pos)
{
    LOG_verbose << "Async read start";
    dst->resize(len + pad);

    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::READ;
    context->posOfBuffer = pos;
    context->pad = pad;
    context->dataBuffer = (byte*)dst->data();
    context->dataBufferLen = len;
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->fa = this;

    if (!asyncopenf())
    {
        LOG_err << "Error in asyncopenf";
        context->failed = true;
        context->retry = this->retry;
        context->finished = true;
        context->userCallback(context->userData);
        return context;
    }

    asyncsysread(context);
    return context;
}

void FileAccess::asyncsysread(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::asyncfwrite(const byte* data, unsigned len, m_off_t pos)
{
    LOG_verbose << "Async write start";

    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::WRITE;
    context->posOfBuffer = pos;
    context->dataBufferLen = len;
    context->dataBuffer = const_cast<byte*>(data);
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->fa = this;

    asyncsyswrite(context);
    return context;
}

void FileAccess::asyncsyswrite(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::newasynccontext()
{
    return new AsyncIOContext();
}

bool FileAccess::fread(string* dst, unsigned len, unsigned pad, m_off_t pos)
{
    if (!openf())
    {
        return false;
    }

    bool r;

    dst->resize(len + pad);

    if ((r = sysread((byte*)dst->data(), len, pos)))
    {
        memset((char*)dst->data() + len, 0, pad);
    }

    closef();

    return r;
}

bool FileAccess::frawread(byte* dst, unsigned len, m_off_t pos, bool caller_opened)
{
    if (!caller_opened && !openf())
    {
        return false;
    }

    bool r = sysread(dst, len, pos);

    if (!caller_opened)
    {
        closef();
    }

    return r;
}

AsyncIOContext::~AsyncIOContext()
{
    finish();

    // AsyncIOContext objects must be deleted before the FileAccess object
    if (op == AsyncIOContext::READ)
    {
        fa->asyncclosef();
    }
}

void AsyncIOContext::finish()
{
    if (!finished)
    {
        while (!finished)
        {
            waiter->init(NEVER);
            waiter->wait();
        }

        // We could have been consumed and external event
        waiter->notify();
    }
}

FileInputStream::FileInputStream(FileAccess *fileAccess)
{
    this->fileAccess = fileAccess;
    this->offset = 0;
}

m_off_t FileInputStream::size()
{
    return fileAccess->size;
}

bool FileInputStream::read(byte *buffer, unsigned size)
{
    if (!buffer)
    {
        if ((offset + size) <= fileAccess->size)
        {
            offset += size;
            return true;
        }

        LOG_warn << "Invalid seek on FileInputStream";
        return false;
    }

    if (fileAccess->frawread(buffer, size, offset, true))
    {
        offset += size;
        return true;
    }

    LOG_warn << "Invalid read on FileInputStream";
    return false;
}

bool LocalPath::empty() const
{
    return localpath.empty();
}

void LocalPath::clear()
{
    localpath.clear();
}

void LocalPath::erase(size_t pos, size_t count)
{
    localpath.erase(pos, count);
}

void LocalPath::truncate(size_t bytePos)
{
    localpath.resize(bytePos);
}

LocalPath LocalPath::leafName(separator_t localseparator) const
{
    auto p = localpath.find_last_of(localseparator);
    p = p == string::npos ? 0 : p + 1;
    LocalPath result;
    result.localpath = localpath.substr(p, localpath.size() - p);
    return result;
}

void LocalPath::append(const LocalPath& additionalPath)
{
    localpath.append(additionalPath.localpath);
}

std::string LocalPath::platformEncoded() const
{
#ifdef WIN32
    // this function is typically used where we need to pass a file path to the client app, which expects utf16 in a std::string buffer
    // some other backwards compatible cases need this format also, eg. serialization
    std::string outstr;
    outstr.resize(localpath.size() * sizeof(wchar_t));
    memcpy(const_cast<char*>(outstr.data()), localpath.data(), localpath.size() * sizeof(wchar_t));
    return outstr;
#else
    // for non-windows, it's just the same utf8 string we use anyway
    return localpath;
#endif
}


void LocalPath::appendWithSeparator(const LocalPath& additionalPath, bool separatorAlways, separator_t localseparator)
{
    if (separatorAlways || localpath.size())
    {
        // still have to be careful about appending a \ to F:\ for example, on windows, which produces an invalid path
        if (!endsInSeparator(localseparator))
        {
            localpath.append(1, localseparator);
        }
    }

    localpath.append(additionalPath.localpath);
}

void LocalPath::prependWithSeparator(const LocalPath& additionalPath, separator_t localseparator)
{
    // no additional separator if there is already one after
    if (!localpath.empty() && localpath[0] != localseparator)
    {
        // no additional separator if there is already one before

        if (!additionalPath.endsInSeparator(localseparator))
        {
            localpath.insert(0, 1, localseparator);
        }
    }
    localpath.insert(0, additionalPath.localpath);
}

void LocalPath::trimNonDriveTrailingSeparator(separator_t localseparator)
{
    if (endsInSeparator(localseparator))
    {
        // ok so the last character is a directory separator.  But don't remove it for eg. F:\ on windows
        #ifdef WIN32
        if (localpath.size() > 1 &&
            localpath[localpath.size() - 2] == L':')
        {
            return;
        }
        #endif

        localpath.resize(localpath.size() - 1);
    }
}

bool LocalPath::findNextSeparator(size_t& separatorBytePos, separator_t localseparator) const
{
    separatorBytePos = localpath.find(localseparator, separatorBytePos);
    return separatorBytePos != string::npos;
}

bool LocalPath::findPrevSeparator(size_t& separatorBytePos, const FileSystemAccess& fsaccess) const
{
    separatorBytePos = localpath.rfind(fsaccess.localseparator, separatorBytePos);
    return separatorBytePos != string::npos;
}

bool LocalPath::endsInSeparator(separator_t localseparator) const
{
    return !localpath.empty() && localpath.back() == localseparator;
}

bool LocalPath::beginsWithSeparator(separator_t localseparator) const
{
    return !localpath.empty() && localpath.front() == localseparator;
}

size_t LocalPath::getLeafnameByteIndex(const FileSystemAccess& fsaccess) const
{
    size_t p = localpath.size();

    while (p && (p -= 1))
    {
        if (localpath[p] == fsaccess.localseparator)
        {
            p += 1;
            break;
        }
    }
    return p;
}

bool LocalPath::backEqual(size_t bytePos, const LocalPath& compareTo) const
{
    auto n = compareTo.localpath.size();
    return bytePos + n == localpath.size() && !localpath.compare(bytePos, n, compareTo.localpath);
}

LocalPath LocalPath::subpathFrom(size_t bytePos) const
{
    LocalPath result;
    result.localpath = localpath.substr(bytePos);
    return result;
}

void LocalPath::ensureWinExtendedPathLenPrefix()
{
#if defined(_WIN32) && !defined(WINDOWS_PHONE)
    if (!PathIsRelativeW(localpath.c_str()) && ((localpath.size() < 2) || memcmp(localpath.data(), L"\\\\", 4)))
    {
        localpath.insert(0, L"\\\\?\\", 4);
    }
#endif
}

LocalPath LocalPath::subpathTo(size_t bytePos) const
{
    LocalPath p;
    p.localpath = localpath.substr(0, bytePos);
    return p;
}


LocalPath LocalPath::insertFilenameCounter(unsigned counter, const FileSystemAccess& fsaccess)
{
    size_t dotindex = localpath.find_last_of('.');
    size_t sepindex = localpath.find_last_of(fsaccess.localseparator);

    LocalPath result, extension;

    if (dotindex == string::npos || (sepindex != string::npos && sepindex > dotindex))
    {
        result.localpath = localpath;
    }
    else
    {
        result.localpath = localpath.substr(0, dotindex);
        extension.localpath = localpath.substr(dotindex);
    }

    ostringstream oss;
    oss << " (" << counter << ")";

    result.localpath += LocalPath::fromPath(oss.str(), fsaccess).localpath + extension.localpath;
    return result;
}


string LocalPath::toPath(const FileSystemAccess& fsaccess) const
{
    string path;
    fsaccess.local2path(&localpath, &path);
    return path;
}

string LocalPath::toName(const FileSystemAccess& fsaccess) const
{
    string name = toPath(fsaccess);
    fsaccess.unescapefsincompatible(&name);
    return name;
}

LocalPath LocalPath::fromPath(const string& path, const FileSystemAccess& fsaccess)
{
    LocalPath p;
    fsaccess.path2local(&path, &p.localpath);
    return p;
}

LocalPath LocalPath::fromName(string path, const FileSystemAccess& fsaccess, FileSystemType fsType)
{
    fsaccess.escapefsincompatible(&path, fsType);
    return fromPath(path, fsaccess);
}

LocalPath LocalPath::fromPlatformEncoded(string path)
{
#if defined(_WIN32)
#ifdef DEBUG
    if (path.size() % 2)
    {
        assert(!(path.size() % 2));
    }
#endif
    LocalPath p;
    p.localpath.resize(path.size() / sizeof(wchar_t));
    memcpy(p.localpath.data(), path.data(), p.localpath.size() * sizeof(wchar_t));
    return p;
#else
    LocalPath p;
    p.localpath = std::move(path);
    return p;
#endif
}

#if defined(_WIN32)
LocalPath LocalPath::fromPlatformEncoded(wstring&& wpath)
{
    LocalPath p;
    p.localpath = std::move(wpath);
    return p;
}

wchar_t LocalPath::driveLetter()
{
    auto drivepos = localpath.find(L':');
    return drivepos == wstring::npos || drivepos < 1 ? 0 : localpath[drivepos-1];
}
#endif


LocalPath LocalPath::tmpNameLocal(const FileSystemAccess& fsaccess)
{
    LocalPath lp;
    fsaccess.tmpnamelocal(lp);
    return lp;
}

int LocalPath::compare(const LocalPath& rhs) const
{
    return detail::localCompare(
             unicodeCodepointIterator(localpath),
             unicodeCodepointIterator(rhs.localpath),
             detail::identity);
}

int LocalPath::compare(const string& rhs) const
{
    return detail::remoteCompare(
             unicodeCodepointIterator(localpath),
             unicodeCodepointIterator(rhs),
             detail::identity);
}

int LocalPath::ciCompare(const LocalPath& rhs) const
{
    return detail::localCompare(
             unicodeCodepointIterator(localpath),
             unicodeCodepointIterator(rhs.localpath),
             Utils::toUpper);
}

int LocalPath::ciCompare(const string& rhs) const
{
    return detail::remoteCompare(
             unicodeCodepointIterator(localpath),
             unicodeCodepointIterator(rhs),
             Utils::toUpper);
}

int LocalPath::fsCompare(const LocalPath& rhs, FileSystemType fsType) const
{
    if (isCaseInsensitive(fsType))
    {
        return ciCompare(rhs);
    }

    return compare(rhs);
}

int LocalPath::fsCompare(const string& rhs, FileSystemType fsType) const
{
    if (isCaseInsensitive(fsType))
    {
        return ciCompare(rhs);
    }

    return compare(rhs);
}

bool LocalPath::isContainingPathOf(const LocalPath& path, separator_t localseparator, size_t* subpathIndex) const
{
    if (path.localpath.size() >= localpath.size()
        && !path.localpath.compare(0, localpath.size(), localpath.data(), localpath.size()))
    {
       if (path.localpath.size() == localpath.size())
       {
           if (subpathIndex) *subpathIndex = localpath.size();
           return true;
       }
       else if (path.localpath[localpath.size()] == localseparator)
       {
           if (subpathIndex) *subpathIndex = localpath.size() + 1;
           return true;
       }
       else if (!localpath.empty() &&
                path.localpath[localpath.size() - 1] == localseparator)
       {
           if (subpathIndex) *subpathIndex = localpath.size();
           return true;
       }
    }
    return false;
}

bool LocalPath::nextPathComponent(size_t& subpathIndex, LocalPath& component, separator_t localseparator) const
{
    while (subpathIndex < localpath.size() && localpath[subpathIndex] == localseparator)
    {
        ++subpathIndex;
    }
    size_t start = subpathIndex;
    if (start >= localpath.size())
    {
        return false;
    }
    else if (findNextSeparator(subpathIndex, localseparator))
    {
        component.localpath = localpath.substr(start, subpathIndex - start);
        return true;
    }
    else
    {
        component.localpath = localpath.substr(start, localpath.size() - start);
        subpathIndex = localpath.size();
        return true;
    }
}

ScopedLengthRestore::ScopedLengthRestore(LocalPath& p)
    : path(p)
    , length(path.getLength())
{
}
ScopedLengthRestore::~ScopedLengthRestore()
{
    path.setLength(length);
};

} // namespace

