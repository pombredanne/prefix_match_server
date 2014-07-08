#include "prefixmatch.h"
#include "config.h"
#include "log.h"

static int g_reloading = 0;
static int g_exiting = 0;
static pthread_rwlock_t  rwlock = PTHREAD_RWLOCK_INITIALIZER;

#define MAX_FILE_LEN 256
#define INITIAL_HASH_SIZE 100000
#define DEFAULT_CHINESE_MAP "./chinese"
#define DEFAULT_INPUT_RANK_FILE "./input"
#define DEFAULT_OUTPUT_INDEX "./index"

class NodeItem
{
public:
    inline bool operator<(const NodeItem &v1) const
    {
        if (this->strName < v1.strName)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    friend bool rank_compare(const NodeItem &s1, const NodeItem &s2);

    string strName;
    float  fRank;
};

bool rank_compare(const NodeItem &s1, const NodeItem &s2)
{
    return s1.fRank < s2.fRank;
}

class string_array : public std::vector<NodeItem>
{
public:
    friend dastrie::itail &operator>>(dastrie::itail &is, string_array &obj)
    {
        obj.clear();

        NodeItem item;
        uint32_t n;
        is >> n;
        for (uint32_t i = 0; i < n; ++i)
        {
            is >> item.strName;
            is >> item.fRank;
            obj.push_back(item);
        }
        return is;
    }

    friend dastrie::otail &operator<<(dastrie::otail &os, const string_array &obj)
    {
        os << (uint32_t)obj.size();
        for (size_t i = 0; i < obj.size(); ++i)
        {
            os << obj[i].strName;
            os << obj[i].fRank;
        }
        return os;
    }
};

typedef dastrie::trie<string_array> trie_type;
typedef unordered_map<string, vector<string> > hashMap;
typedef struct index
{
    char index_file[MAX_FILE_LEN];
    unsigned char *mem;
    int  fsize;
    trie_type g_dasTrieObj;
} indexobj;
hashMap chinese_map(INITIAL_HASH_SIZE);
indexobj g_index;

int init_index(char *index_file, indexobj &index);
int deinit_index(indexobj &index);

int mmap_file(int fd, unsigned char **mems)
{
    struct stat st;
    unsigned char *mem;
    unsigned fsize = 0;
    if (fstat(fd, &st) < 0)
    {
        log_debug(LOG_ERR, "call fstat failed\n");
        return -1;
    }
    fsize = st.st_size;
    if (fsize == 0)
    {
        return -1;
    }
    mem = (unsigned char *)mmap(NULL, fsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        log_debug(LOG_ERR, "call mmap failed, %d\n", errno);
        return -1;
    }
    *mems = mem;
    return fsize;
}

int read_chinese_map(const string &strFile, hashMap &chinese_map)
{
    int file_strLine = 0;
    ifstream inf(strFile.c_str());
    if (!inf.is_open())
    {
        return -1;
    }

    string strLine;
    while (getline(inf, strLine))
    {
        strLine = trim(strLine, " \t\r", 1);
        vector<string> vResult = sepstr(strLine, " ");
        if (vResult.size() < 2)
        {
            continue;
        }
        chinese_map[vResult[0]] = vector<string>(++(vResult.begin()), vResult.end());
        ++file_strLine;
    }
    log_debug(LOG_ERR, "file count %d in file %s\n", file_strLine, strFile.c_str());
    return 0;
}

void get_all_results(const vector< vector<string> > &vecAll, vector<string> &vOut)
{
    if (vecAll.size() == 0)
    {
        return;
    }

    vOut = vecAll[0];

    size_t uCount = vecAll.size();
    for (size_t i = 1 ; i < uCount ; ++i)
    {
        size_t uOutCount = vOut.size();
        size_t uItemCount = vecAll[i].size();
        for (size_t j = 0 ; j < uItemCount - 1 ; ++j)
        {
            for (size_t k = 0 ; k < uOutCount ; ++k)
            {
                vOut.push_back(vOut[k]);
            }
        }

        for (size_t j = 0 ; j < uItemCount ; ++j)
        {
            for (size_t k = j * uOutCount ; k < (j + 1) * uOutCount ; ++k)
            {
                vOut[k].append(vecAll[i][j]);
            }
        }
    }
}

int all_english_char(const string &strIn)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(strIn.c_str());
    const unsigned char *pEnd = p + strIn.size();
    while (p < pEnd)
    {
        if (*p >= 0x80)
        {
            return 0;
        }
        p++;
    }
    return 1;
}

void convert_to_letters(const string &strIn, hashMap &hz2pyTable, vector<string> &vOut)
{
    vector< vector<string> > vecAll;
    vOut.clear();

    if (all_english_char(strIn))
    {
        vOut.push_back(strIn);
        return;
    }

    const char *p = strIn.c_str();
    const char *pEnd = p + strIn.size();
    while (p < pEnd)
    {
        size_t s = getUTF8Len(p);
        string strKey(p, s);
        hashMap::iterator iter = hz2pyTable.find(strKey);
        if (iter != hz2pyTable.end())
        {
            vecAll.push_back(iter->second);
        }
        p += s;
    }
    get_all_results(vecAll, vOut);
}

static void filter_result(const vector<NodeItem> &results, const vector<string> &filter_rule, vector<string> &vecResult, int nMaxNumToGet)
{
    set<NodeItem> resultSet;
    vector<NodeItem> filter_results;
    size_t resultnum = results.size();
    size_t rulenum = filter_rule.size();
    for (int i = 0 ; i < resultnum; ++i)
    {
        bool bFound = true;
        for (size_t j = 0 ; j < rulenum ; ++j)
        {
            if (string::npos == results[i].strName.find(filter_rule[j]))
            {
                bFound = false;
                break;
            }
        }
        if (bFound)
        {
            resultSet.insert(results[i]);
        }
    }

    resultnum = 0;
    for (set<NodeItem>::iterator it = resultSet.begin(); it != resultSet.end(); ++it)
    {
        filter_results.push_back(*it);
        resultnum++;
    }

    sort(filter_results.begin(), filter_results.end(), rank_compare);
    for (size_t i = 0 ; i < resultnum && i < nMaxNumToGet; ++i)
    {
        vecResult.push_back(filter_results[i].strName);
    }
    return;
}

int Query(const string &strQuery, vector<string> &vecResult, int nMaxNumToGet)
{
    size_t i = 0;
    const char *p = strQuery.c_str();
    const char *pEnd = p + strQuery.size();
    vector<string> vChinese;
    vector<trie_type::KeyValuePair> vResultTmp;
    vector<NodeItem> vTmpNode;
    vector<string> vLetters;

    while (p < pEnd)
    {
        i = getUTF8Len(p);
        if (i == 1)
        {
            ++p;
            continue;
        }
        vChinese.push_back(string(p, i));
        p += i;
    }

    convert_to_letters(strQuery, chinese_map, vLetters);
    if (vLetters.size() == 0)
    {
        log_debug(LOG_ERR, "the size of letters is 0\n");
        return -1;
    }

    pthread_rwlock_rdlock(&rwlock);
    for (vector<string>::iterator it = vLetters.begin(); it != vLetters.end(); ++it)
    {
        g_index.g_dasTrieObj.getChildren(it->c_str(), vResultTmp, g_settings.max_depth);
    }
    pthread_rwlock_unlock(&rwlock);
    for (vector<trie_type::KeyValuePair>::iterator vecIt = vResultTmp.begin(); vecIt != vResultTmp.end(); ++vecIt)
    {
        for (string_array::iterator it = vecIt->value.begin(); it != vecIt->value.end(); ++it)
        {
            vTmpNode.push_back(*it);
        }
    }

    filter_result(vTmpNode, vChinese, vecResult, nMaxNumToGet);
    return 0;
}

int init_index(char *index_file, indexobj &index)
{
    int ifd = open(index_file, O_RDWR);
    if (ifd < 0)
    {
        log_debug(LOG_ERR, "open file %s failed\n", index_file);
        return -1;
    }
    unsigned char *mem = NULL;
    int fsize = mmap_file(ifd, &mem);
    if (fsize == -1)
    {
        log_debug(LOG_ERR, "mmap file %s failed\n", index_file);
        close(ifd);
        return -1;
    }
    close(ifd);

    log_debug(LOG_ERR, index.index_file, MAX_FILE_LEN, "%s", index_file);
    index.mem = mem;
    index.fsize = fsize;

    //load index data into g_dasTrieObj.
    if (index.g_dasTrieObj.assign((const char *)mem, fsize) == 0)
    {
        deinit_index(index);
        return -1;
    }
    return 0;
}

int deinit_index(indexobj &index)
{
    int ret = munmap(index.mem, index.fsize);
    index.mem = NULL;
}

int Init_Index(char *py_file, char *index_file)
{
    int ret = 0;
    if (0 != read_chinese_map(py_file, chinese_map))
    {
        return -1;
    }
    if (init_index(index_file, g_index) != 0)
    {
        return -1;
    }
    return 0;
}

int Deinit_Index()
{
    deinit_index(g_index);
    return 0;
}

int exiting()
{
    g_exiting = 1;
}

int Reload_index(char *newindex_file)
{
    if (newindex_file == NULL || strlen(newindex_file) == 0)
    {
        log_debug(LOG_NOTICE, "the new index file can not be found, use %s instead\n", g_settings.index_path);
        newindex_file = g_settings.index_path;
    }

    int ret = 0;
    indexobj new_index;
    indexobj prev_index = g_index;

    if (g_reloading == 1)
    {
        log_debug(LOG_NOTICE, "it is in reloading now\n");
        return -1;
    }
    g_reloading = 1;

    //load new index
    ret = init_index(newindex_file, new_index);
    if (ret != 0)
    {
        log_debug(LOG_NOTICE, "call init index error, ret:%d\n", ret);
        g_reloading = 0;
        return -1;
    }

    //switch it
    pthread_rwlock_wrlock(&rwlock);
    g_index = new_index;
    pthread_rwlock_unlock(&rwlock);

    //release old index and reset flag
    deinit_index(prev_index);
    g_reloading = 0;

    log_debug(LOG_NOTICE, "read new index %s successfully\n", newindex_file);
    return 0;
}

int Get(string line, vector<string> &vRes)
{
    if (g_exiting == 1)
    {
        vRes.clear();
        return 0;
    }

    int ret = 0;
    if (line.size() == 0)
    {
        log_debug(LOG_ERR, "input empty request\n");
        return -1;
    }
    line = trim(line, " \t\r", 1);
    ret = Query(line, vRes, g_settings.max_depth);
    log_debug(LOG_NOTICE, "input key: %s, return: %d\n", line.c_str(), ret);
    return ret;
}

#ifdef TEST
//g++ prefixmatch.cpp util.cpp config.cpp  -DTEST -g --std=c++0x -lpthread
int main(int argc, char **argv)
{
    if (argc != 4)
    {
        cout << "wrong input" << endl;
        return -1;
    }
    char *py = argv[1];
    char *index = argv[2];
    char *file = argv[3];

    int ret = Init_Index(py, index);
    if (ret != 0)
    {
        cout << "init index error" << endl;
        return -1;
    }

    string line;
    std::ifstream ifs(file);
    ifs >> line;
    cout << "line: " << line << endl;
    ifs.close();

    vector<string> vRes;
    ret = Get(line, vRes);
    if (ret != 0)
    {
        cout << "call Get error" << endl;
        return -1;
    }

    for (int i = 0; i < vRes.size(); i++)
    {
        cout << vRes[i] << " ";
    }
    cout << endl;

    return 0;
}
#endif

