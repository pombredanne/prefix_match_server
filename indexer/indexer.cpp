#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <algorithm>
#include <unordered_map>

#include "dastrie.h"
#include "util.h"

using namespace std;
using std::unordered_map;

#define MAX_FILE_LEN 256
#define INITIAL_HASH_SIZE 100000
#define DEFAULT_CHINESE_MAP "./chinese"
#define DEFAULT_INPUT_RANK_FILE "./input"
#define DEFAULT_OUTPUT_INDEX "./index"

typedef struct conf
{
    char chinese_map_file[MAX_FILE_LEN];
    char input_rank_file[MAX_FILE_LEN];
    char output_index_file[MAX_FILE_LEN];
} conf;
conf g_conf;

void usage();

#define CONF_SET_STR_VALUE(field, value)    \
    do{ \
        snprintf(g_conf.field, sizeof(g_conf.field), "%s", value);\
    }while(0)
#define CONF_PRINT_STR(field) \
    do { \
        printf("field %s\n", g_conf.field);\
    }while(0)
#define CONF_CHECK(field) \
    do { \
        int ret = 0;\
        ret = file_exist(g_conf.field);\
        if(ret==0) {\
            printf("file %s does not exist\n", g_conf.field);\
            printf("\n");\
            usage(); \
            exit(-1); \
        } \
    }while(0)

void usage()
{
    printf("The Tool is used to build index for prefix match.\n");
    printf("index [-C-I-O-h]\n");
    printf("\t-h\t print usage\n");
    printf("\t-C\t the Chinese to letter convert file\n");
    printf("\t-I\t Input rank file\n");
    printf("\t-O\t Output index file\n");
}

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

typedef dastrie::builder<std::string, string_array> builder_type;
typedef builder_type::record_type record_type;
typedef unordered_map<string, vector<string> > hashMap;
hashMap chinese_map(INITIAL_HASH_SIZE);

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
    printf("file count %d in file %s\n", file_strLine, strFile.c_str());
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
    int all_chinese_flag = 1;

    vector< vector<string> > vecAll;
    vector< vector<string> > vecFC; //First Character
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
        if (s <= 1)
        {
            all_chinese_flag = 0;
        }
        string strKey(p, s);
        hashMap::iterator iter = hz2pyTable.find(strKey);
        if (iter != hz2pyTable.end())
        {
            vecAll.push_back(iter->second);
            vector<string> vTmp;
            for (size_t i = 0 ; i < iter->second.size() ; ++i)
            {
                vTmp.push_back(iter->second[i].substr(0, 1));
            }
            vecFC.push_back(vTmp);
        }
        p += s;
    }
    get_all_results(vecAll, vOut);
    if (all_chinese_flag == 1)
    {
        vector<string> vTmp;
        get_all_results(vecFC, vTmp);
        vOut.insert(vOut.end(), vTmp.begin(), vTmp.end());
    }
}

int create_index(const char *input_rank_file, char *output_index_file)
{
    string strLine;
    NodeItem item;
    vector<string> vChinese;
    map<string, string_array> mLetterToItems;

    string strInput(input_rank_file);
    string strOutput(output_index_file);

    ifstream infile(strInput);
    if (!infile.is_open())
    {
        printf("failed to open file %s\n", input_rank_file);
        return -1;
    }

    while (getline(infile, strLine))
    {
        vector<string> vResult = sepstr(strLine, "\t");
        size_t uCount = vResult.size();
        if (2 == uCount)
        {
            item.strName = vResult[0];
            item.fRank = atof(vResult[1].c_str());
        }
        if (uCount != 2)
        {
            printf("the line must have 2 fields\n");
            continue;
        }
        convert_to_letters(strLine, chinese_map, vChinese);
        if (vChinese.size() > 0)
        {
            for (vector<string>::iterator it = vChinese.begin(); it != vChinese.end(); ++it)
            {
                mLetterToItems[*it].push_back(item);
            }
        }
        vChinese.clear();
    }
    infile.close();

    vector<record_type> allRecords;
    allRecords.reserve(mLetterToItems.size());
    for (map<string, string_array>::iterator it = mLetterToItems.begin(); it != mLetterToItems.end(); ++it)
    {
        record_type record;
        record.key = it->first;
        record.value = it->second;

        allRecords.push_back(record);
    }

    builder_type builder;
    builder.build(&allRecords[0], &allRecords[allRecords.size() - 1]);
    std::ofstream ofs(strOutput, std::ios::binary);
    builder.write(ofs);
    ofs.close();
    return 0;
}

void init_default_conf()
{
    memset(&g_conf, 0, sizeof(conf));
    CONF_SET_STR_VALUE(chinese_map_file, DEFAULT_CHINESE_MAP);
    CONF_SET_STR_VALUE(input_rank_file, DEFAULT_INPUT_RANK_FILE);
    CONF_SET_STR_VALUE(output_index_file, DEFAULT_OUTPUT_INDEX);
}

void check_conf()
{
    CONF_PRINT_STR(chinese_map_file);
    CONF_PRINT_STR(input_rank_file);
    CONF_PRINT_STR(output_index_file);

    CONF_CHECK(chinese_map_file);
    CONF_CHECK(input_rank_file);
}

int main(int argc, char **argv)
{
    int ret  = 0;
    char c;

    /* set the default values */
    init_default_conf();

    /* arguments process */
    while ((c = getopt(argc, argv, "C:I:O:h")) != -1)
    {
        switch (c)
        {
            case 'h':
                usage();
                exit(0);
            case 'C':
                CONF_SET_STR_VALUE(chinese_map_file, optarg);
                break;
            case 'I':
                CONF_SET_STR_VALUE(input_rank_file, optarg);
                break;
            case 'O':
                CONF_SET_STR_VALUE(output_index_file, optarg);
                break;
            default:
                usage();
        }
    }

    check_conf();

    if (0 != read_chinese_map(g_conf.chinese_map_file, chinese_map))
    {
        printf("failed in read_chinese_map\n");
        return -1;
    }

    ret = create_index(g_conf.input_rank_file, g_conf.output_index_file);
    if (ret != 0)
    {
        printf("create_index error\n");
        return -1;
    }

    printf("create_index succ\n");
    return 0;
}
