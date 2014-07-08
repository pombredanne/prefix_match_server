#ifndef __UTIL_H__
#define __UTIL_H__

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <inttypes.h>

using namespace std;

typedef unsigned int rel_time_t;

bool safe_strtoull(const char *str, uint64_t *out);
bool safe_strtoll(const char *str, int64_t *out);
bool safe_strtoul(const char *str, uint32_t *out);
bool safe_strtol(const char *str, int32_t *out);

#ifndef HAVE_HTONLL
extern uint64_t htonll(uint64_t);
extern uint64_t ntohll(uint64_t);
#endif

#ifdef __GCC
# define __gcc_attribute__ __attribute__
#else
# define __gcc_attribute__(x)
#endif

/**
 * Vararg variant of perror that makes for more useful error messages
 * when reporting with parameters.
 *
 * @param fmt a printf format
 */
void vperror(const char *fmt, ...)
__gcc_attribute__((format(printf, 1, 2)));

typedef struct token_s
{
    char *value;
    size_t length;
} token_t;


#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 8

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens);


#define REALTIME_MAXDELTA 60*60*24*30

/*
 * given time value that's either unix time or delta from current unix time, return
 * unix time. Use the fact that delta can't exceed one month (and real time value can't
 * be that low).
 */
rel_time_t realtime(const time_t exptime);

#define MAX_KEYWORD_LENGTH 1024

//modify the str
int parse_string_fast(char *str, const char *split, char **dest, int *size);

/*
    delete the 'delchs' in str, this function will modify the source str.
    return the length of deleted str.
*/
int delstr(char *str, const char *delchs);

/*
    get the utf8 len of the string p
*/
size_t getUTF8Len(const char *p);

/*
    separate the "sStr" with the separator "sSep".
    return all separated parts
*/
vector<string> sepstr(const string &sStr, const string &sSep, bool withEmpty = false);

/*
    check the existence of file.
    1 - exist
    0 - not exist
*/
int file_exist(char *file);

int mkdir_recursive(const char *pathname, mode_t mode);
int mysleep_sec(unsigned int second);
int mysleep_millisec(unsigned int millisecond);

string trimright(const string &sStr, const string &s, bool bChar);
string trimleft(const string &sStr, const string &s, bool bChar);
string trim(const string &sStr, const string &s, bool bChar);


#endif
