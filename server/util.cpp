#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "util.h"

string trimleft(const string &sStr, const string &s, bool bChar)
{
    if (sStr.empty())
    {
        return sStr;
    }    /**    * 去掉sStr左边的字符串s    */    if (!bChar)
    {
        if (sStr.length() < s.length())
        {
            return sStr;
        }
        if (sStr.compare(0, s.length(), s) == 0)
        {
            return sStr.substr(s.length());
        }
        return sStr;
    }    /**    * 去掉sStr左边的 字符串s中的字符    */    string::size_type pos = 0;
    while (pos < sStr.length())
    {
        if (s.find_first_of(sStr[pos]) == string::npos)
        {
            break;
        }
        pos++;
    }
    if (pos == 0)
    {
        return sStr;
    }
    return sStr.substr(pos);
}
string trimright(const string &sStr, const string &s, bool bChar)
{
    if (sStr.empty())
    {
        return sStr;
    }    /**    * 去掉sStr右边的字符串s    */    if (!bChar)
    {
        if (sStr.length() < s.length())
        {
            return sStr;
        }
        if (sStr.compare(sStr.length() - s.length(), s.length(), s) == 0)
        {
            return sStr.substr(0, sStr.length() - s.length());
        }
        return sStr;
    }    /**    * 去掉sStr右边的 字符串s中的字符    */    string::size_type pos = sStr.length();
    while (pos != 0)
    {
        if (s.find_first_of(sStr[pos - 1]) == string::npos)
        {
            break;
        }
        pos--;
    }
    if (pos == sStr.length())
    {
        return sStr;
    }
    return sStr.substr(0, pos);
}
string trim(const string &sStr, const string &s, bool bChar)
{
    if (sStr.empty())
    {
        return sStr;
    }    /**    * 将完全与s相同的字符串去掉    */    if (!bChar)
    {
        return trimright(trimleft(sStr, s, false), s, false);
    }
    return trimright(trimleft(sStr, s, true), s, true);
}



string strto(const string &sStr)
{
    return (sStr);
}

vector<string> sepstr(const string &sStr, const string &sSep, bool withEmpty)
{
    vector<string> vt;

    string::size_type pos = 0;
    string::size_type pos1 = 0;

    while (true)
    {
        string s;
        pos1 = sStr.find_first_of(sSep, pos);
        if (pos1 == string::npos)
        {
            if (pos + 1 <= sStr.length())
            {
                s = sStr.substr(pos);
            }
        }
        else if (pos1 == pos)
        {
            s = "";
        }
        else
        {
            s = sStr.substr(pos, pos1 - pos);
            pos = pos1;
        }

        if (withEmpty)
        {
            vt.push_back(strto(s));
        }
        else
        {
            if (!s.empty())
            {
                string tmp = strto(s);
                vt.push_back(tmp);
            }
        }

        if (pos1 == string::npos)
        {
            break;
        }

        pos++;
    }

    return vt;
}

size_t getUTF8Len(const char *p)
{
    if (!p || !*p)
    {
        return 0;
    }

    const unsigned char *pTemp = reinterpret_cast<const unsigned char *>(p);
    if (*pTemp < 0x80)
    {
        return 1;
    }
    else if ((*pTemp >> 5) == 0x6)
    {
        return 2;
    }
    else if ((*pTemp >> 4) == 0xe)
    {
        return 3;
    }
    else if ((*pTemp >> 3) == 0x1e)
    {
        return 4;
    }
    else
    {
        return 0;
    }
}

/* Avoid warnings on solaris, where isspace() is an index into an array, and gcc uses signed chars */
#define xisspace(c) isspace((unsigned char)c)

bool safe_strtoull(const char *str, uint64_t *out)
{
    assert(out != NULL);
    errno = 0;
    *out = 0;
    char *endptr;
    unsigned long long ull = strtoull(str, &endptr, 10);
    if ((errno == ERANGE) || (str == endptr))
    {
        return false;
    }

    if (xisspace(*endptr) || (*endptr == '\0' && endptr != str))
    {
        if ((long long) ull < 0)
        {
            /* only check for negative signs in the uncommon case when
             * the unsigned number is so big that it's negative as a
             * signed number. */
            if (strchr(str, '-') != NULL)
            {
                return false;
            }
        }
        *out = ull;
        return true;
    }
    return false;
}

bool safe_strtoll(const char *str, int64_t *out)
{
    assert(out != NULL);
    errno = 0;
    *out = 0;
    char *endptr;
    long long ll = strtoll(str, &endptr, 10);
    if ((errno == ERANGE) || (str == endptr))
    {
        return false;
    }

    if (xisspace(*endptr) || (*endptr == '\0' && endptr != str))
    {
        *out = ll;
        return true;
    }
    return false;
}

bool safe_strtoul(const char *str, uint32_t *out)
{
    char *endptr = NULL;
    unsigned long l = 0;
    assert(out);
    assert(str);
    *out = 0;
    errno = 0;

    l = strtoul(str, &endptr, 10);
    if ((errno == ERANGE) || (str == endptr))
    {
        return false;
    }

    if (xisspace(*endptr) || (*endptr == '\0' && endptr != str))
    {
        if ((long) l < 0)
        {
            /* only check for negative signs in the uncommon case when
             * the unsigned number is so big that it's negative as a
             * signed number. */
            if (strchr(str, '-') != NULL)
            {
                return false;
            }
        }
        *out = l;
        return true;
    }

    return false;
}

bool safe_strtol(const char *str, int32_t *out)
{
    assert(out != NULL);
    errno = 0;
    *out = 0;
    char *endptr;
    long l = strtol(str, &endptr, 10);
    if ((errno == ERANGE) || (str == endptr))
    {
        return false;
    }

    if (xisspace(*endptr) || (*endptr == '\0' && endptr != str))
    {
        *out = l;
        return true;
    }
    return false;
}

void vperror(const char *fmt, ...)
{
    int old_errno = errno;
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    if (vsnprintf(buf, sizeof(buf), fmt, ap) == -1)
    {
        buf[sizeof(buf) - 1] = '\0';
    }
    va_end(ap);

    errno = old_errno;

    perror(buf);
}

#ifndef HAVE_HTONLL
static uint64_t mc_swap64(uint64_t in)
{
#ifdef ENDIAN_LITTLE
    /* Little endian, flip the bytes around until someone makes a faster/better
    * way to do this. */
    int64_t rv = 0;
    int i = 0;
    for (i = 0; i < 8; i++)
    {
        rv = (rv << 8) | (in & 0xff);
        in >>= 8;
    }
    return rv;
#else
    /* big-endian machines don't need byte swapping */
    return in;
#endif
}

uint64_t ntohll(uint64_t val)
{
    return mc_swap64(val);
}

uint64_t htonll(uint64_t val)
{
    return mc_swap64(val);
}
#endif

size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens)
{
    char *s, *e;
    size_t ntokens = 0;
    size_t len = strlen(command);
    unsigned int i = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    s = e = command;
    for (i = 0; i < len; i++)
    {
        if (*e == ' ')
        {
            if (s != e)
            {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
                if (ntokens == max_tokens - 1)
                {
                    e++;
                    s = e; /* so we don't add an extra token */
                    break;
                }
            }
            s = e + 1;
        }
        e++;
    }

    if (s != e)
    {
        tokens[ntokens].value = s;
        tokens[ntokens].length = e - s;
        ntokens++;
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

int parse_string_fast(char *str, const char *split, char **dest, int *size)
{
    *size = 0;
    char *head = str;
    while (head != NULL)
    {
        if (*size >= MAX_KEYWORD_LENGTH)
        {
            return 0;
        }
        str = strchr(head, *split);
        if (str != NULL)
        {
            *str = '\0';
        }
        *dest = head;
        dest++;
        *size = *size + 1;
        if (str != NULL)
        {
            head = str + 1;
        }
        else
        {
            return 0;
        }
    }
    return 0;
}

int delstr(char *str, const char *delchs)
{
    int size = strlen(str);
    int span = 0;
    for (int i = 0; i < size;)
    {
        if (strchr(delchs, str[i]) != NULL)
        {
            span++;
            i++;
            continue;
        }
        else if (span > 0)
        {
            str[i - span] = str[i];
            i++;
        }
        else
        {
            i++;
            continue;
        }
    }
    for (int i = size - span; i < size; i++)
    {
        str[i] = '\0';
    }
    return size - span;
}

int file_exist(char *file)
{
    if (access(file, F_OK) == 0)
    {
        return 1;
    }
    return 0;
}

int mkdir_recursive(const char *pathname, mode_t mode)
{
    struct stat res;
    memset(&res, 0, sizeof(res));
    if (stat(pathname, &res) == 0 && S_ISDIR(res.st_mode))
    {
        if (access(pathname, W_OK) != 0)
        {
            return 0;
        }
        else
        {
            return 1;
        }
    }

    char *pos = (char *)pathname;
    char *head = (char *)pathname;
    int total_len = strlen(pathname);
    int tmp_len = 0;
    while (pos != NULL)
    {
        if (tmp_len > total_len)
        {
            return 1;
        }

        pos = strchr(head + tmp_len, '/');
        if (NULL == pos)
        {
            return -1 != mkdir(head, mode);
        }
        else
        {
            char tmp = *pos;
            *pos = '\0';
            mkdir(head, mode);
            *pos = tmp;
        }

        tmp_len = pos - head + 1;
    }
    return 1;
}

int mysleep_sec(unsigned int second)
{
    struct timeval t_timeval;
    t_timeval.tv_sec = second;
    t_timeval.tv_usec = 0;
    select(0, NULL, NULL, NULL, &t_timeval);
    return 0;
}

int mysleep_millisec(unsigned int millisecond)
{
    struct timeval t_timeval;
    t_timeval.tv_sec = 0;
    t_timeval.tv_usec = 1000 * millisecond;
    select(0, NULL, NULL, NULL, &t_timeval);
    return 0;
}
