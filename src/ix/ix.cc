
#include "ix.h"

#include <stdlib.h>
#include <sys/_types/_size_t.h>
#include <sys/stat.h>
#include <__tuple>
#include <cstring>
#include <iostream>
#include <stack>
#include <tuple>
#include <utility>

#include "../rbf/pfm.h"

IndexManager* IndexManager::_index_manager = 0;

IndexManager* IndexManager::instance()
{
    if(!_index_manager)
        _index_manager = new IndexManager();

    return _index_manager;
}

bool fileExists(const std::string& filename)
{
    struct stat buf;
    if (stat(filename.c_str(), &buf) != -1)
    {
        return true;
    }
    return false;
}

IndexManager::IndexManager() {
    pagedFileManager = PagedFileManager::instance();

    if(fileExists("root_nodes")) {
        fstream f;
        f.open("root_nodes", ios_base::in);
        if(f.is_open()) {
            int numOfEntries;
            f>>numOfEntries;
            for(int i=0;i<numOfEntries;i++) {
                string file;
                int rootPage;
                f >> file >> rootPage;
                indexRootNodeMap[file] = rootPage;
            }
            f.close();
        }
    }
}

IndexManager::~IndexManager() {
//    indexRootNodeMap.clear();
}

RC IndexManager::createFile(const string &fileName) {
    return pagedFileManager -> createFile(fileName);
}

RC IndexManager::destroyFile(const string &fileName)
{
    return pagedFileManager -> destroyFile(fileName);
}

RC IndexManager::openFile(const string &fileName, IXFileHandle &ixfileHandle)
{
    RC result;
    result = pagedFileManager -> openFile(fileName, ixfileHandle);
    ixfileHandle.fileName = fileName;
    return result;
}

RC IndexManager::closeFile(IXFileHandle &ixfileHandle)
{
    return pagedFileManager -> closeFile(ixfileHandle);
}

unsigned short IndexManager::getFreeSpaceFromPage(void* pageData) const {
    return *(unsigned short*)((char*)pageData + PAGE_SIZE - FREE_SPACE_OFFSET);
}

int IndexManager::getPageTypeFromPage(void* pageData) {
    return *(unsigned short*)((char*)pageData + PAGE_SIZE - PAGE_TYPE_OFFSET);
}

PageNum IndexManager::getNextSiblingPage(void* pageData) {
    return *(PageNum*)((char*)pageData + PAGE_SIZE - NEXT_PAGE_OFFSET);
}

RC IndexManager::setNextSiblingPage(void* pageData, PageNum pageNum) {
    *(PageNum*)((char*)pageData + PAGE_SIZE - NEXT_PAGE_OFFSET) = pageNum;
    return 0;
}

int IndexManager::getEndOfRecordOffsetFromPage(void*pageData) {
    int pageType = getPageTypeFromPage(pageData);
    if(pageType == LEAF) {
        return (PAGE_SIZE - NEXT_PAGE_OFFSET - getFreeSpaceFromPage(pageData));
    } else {
        return (PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData));
    }
}

RC IndexManager::createLeafEntry(const Attribute &attribute, const void* key, const RID &rid, void* record, int &recordLen) {
    switch(attribute.type) {
        case TypeInt: {
            recordLen = sizeof(int) + sizeof(RID);
            memcpy((char*)record, key, sizeof(int));
            memcpy((char*)record + sizeof(int), &rid, sizeof(RID));
            break;
        }
        case TypeReal: {
            recordLen = 4 + sizeof(RID);
            memcpy((char*)record, key, sizeof(float));
            memcpy((char*)record + sizeof(float), &rid, sizeof(RID));
            break;
        }
        case TypeVarChar: {
            cout << "Creating leaf entry" << endl;
            unsigned short varcharLength = *(int *) key;
            cout << "Varchar length: " << varcharLength << endl;
            char *actualRecord = (char *) calloc(varcharLength, 1);
            memcpy(actualRecord, (char *) key + sizeof(int), varcharLength);
            recordLen = sizeof(unsigned short) + varcharLength + sizeof(RID);
            memcpy(record, &varcharLength, sizeof(unsigned short));
            memcpy((char *) record + sizeof(unsigned short), actualRecord, varcharLength);
            memcpy((char *) record + sizeof(unsigned short) + varcharLength, &rid, sizeof(RID));
            cout << "Finally printing the length" <<*(unsigned short*)record<<endl;
            free(actualRecord);
            break;
        }
        default:
            cout << "[ERROR]Invalid attribute type in index" << endl;
            break;
    }
    return -1;
}

RC IndexManager::readLeafEntryAtOffset(int offset, void* pageData, const Attribute &attribute, void* currentOffsetEntry, int &currentOffsetEntryLen) {
    switch(attribute.type) {
        case TypeInt:
            currentOffsetEntryLen = sizeof(int) + sizeof(RID);
            memcpy(currentOffsetEntry, (char*)pageData + offset, currentOffsetEntryLen);
            break;
        case TypeReal:
            currentOffsetEntryLen = sizeof(float) + sizeof(RID);
            memcpy(currentOffsetEntry, (char*)pageData + offset, currentOffsetEntryLen);
            break;
        case TypeVarChar: {
            int varCharLength = *(unsigned short *) ((char *) pageData + offset);
            cout << "Varchar length is as follows: " << varCharLength << endl;
            currentOffsetEntryLen = sizeof(unsigned short) + varCharLength + sizeof(RID);
            memcpy(currentOffsetEntry, (char *) pageData + offset, currentOffsetEntryLen);
            break;
        }
        default:
            cout << "[ERROR]Invalid attribute type in index" << endl;
            break;
    }
    return 0;
}

RC IndexManager::setPageType(void* pageData, unsigned short pageType) {
    *(unsigned short*)((char*)pageData + PAGE_SIZE - PAGE_TYPE_OFFSET) = pageType;
    return 0;
}

RC IndexManager::setFreeSpace(void* pageData, unsigned short freeSpace) {
    *(unsigned short*)((char*)pageData + PAGE_SIZE - FREE_SPACE_OFFSET) = freeSpace;
    return 0;
}

RC IndexManager::squeezeEntryIntoLeaf(void *pageData, const Attribute &attribute, void* entry, int entryLen, const void *key, const RID &rid) {
    int offset = 0;
    bool flag = true;
    int pageEndOffset = getEndOfRecordOffsetFromPage(pageData);
    while(offset <= PAGE_SIZE && flag) {
        void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
        int currentOffsetEntryLen;
        readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntry, currentOffsetEntryLen);
        switch(attribute.type) {
            case TypeInt: {
                if(offset >= pageEndOffset) {
                    memcpy((char *) pageData + offset, entry, entryLen);
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - entryLen);
                    return 0;
                }
                int currentKey = *(int *) currentOffsetEntry;
                if (*(int *) key < currentKey) {
                    int endOfRecordsOffset = PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData);
                    int lengthToShift = endOfRecordsOffset - offset;
                    if(lengthToShift > 0)
                        memmove((char *) pageData + offset + entryLen, (char *) pageData + offset, lengthToShift);
                    memcpy((char *) pageData + offset, entry, entryLen);
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - entryLen);
                    return 0;
                } else {
                    offset += currentOffsetEntryLen;
                }
                break;
            }
            case TypeReal: {
                if(offset >= pageEndOffset) {
                    memcpy((char *) pageData + offset, entry, entryLen);
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - entryLen);
                    return 0;
                }
                float currentKey = *(float *) currentOffsetEntry;
                if (*(float *) key < currentKey) {
                    int endOfRecordsOffset = PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData);
                    int lengthToShift = endOfRecordsOffset - offset;
                    if(lengthToShift > 0)
                        memmove((char *) pageData + offset + entryLen, (char *) pageData + offset, lengthToShift);
                    memcpy((char *) pageData + offset, entry, entryLen);
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - entryLen);
                    return 0;
                } else {
                    offset += currentOffsetEntryLen;
                }
                break;
            }
            case TypeVarChar: {
                if(offset >= pageEndOffset) {
                    cout << "In squeeze inserting at end " << rid.pageNum << " " << rid.slotNum << " " << *(unsigned short*)entry << " "  <<(char*)entry + 2 << " " << entryLen << " offset " << offset <<endl;
                    if(rid.pageNum == 610) {
//                        offset -= 1;
                    }
                    memcpy((char *) pageData + offset, entry, entryLen);
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - entryLen);
                    return 0;
                }
                int varcharLength = *(unsigned short*) currentOffsetEntry;
                int insertKeyLength = *(unsigned short*) entry;
                if(strncmp((char*)entry + sizeof(unsigned short), (char*)currentOffsetEntry + sizeof(unsigned short), varcharLength) < 0) {
                    int endOfRecordsOffset = PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData);
                    int lengthToShift = endOfRecordsOffset - offset;
                    if(lengthToShift > 0)
                        memmove((char *) pageData + offset + entryLen, (char *) pageData + offset, lengthToShift);
                    memcpy((char *) pageData + offset, entry, entryLen);
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - entryLen);
                    return 0;
                } else {
                    offset += currentOffsetEntryLen;
                }
                break;
            }
            default:
                cout << "[ERROR]Invalid attribute type in index" << endl;
                break;
        }
        free(currentOffsetEntry);
    }
    return -1;
}

RC IndexManager::persistIndexRootNodeMap() {
    fstream f;
    f.open("root_nodes", ios_base::out);
    f<<indexRootNodeMap.size()<<endl;
    for(auto it=indexRootNodeMap.begin(); it != indexRootNodeMap.end(); it++) {
        cout << "Persisting it values: " << it->first << " " << it->second << endl;
        f << it->first << " " << it->second << endl;
    }
    f.close();
    return 0;
}

RC IndexManager::splitLeafNode(void *pageData, void* newPageData, const Attribute &attribute, void* entry, int entryLen, const void *key, const RID &rid) {
    //This page is full: implicit assumption since the function is called
    cout << "Splitting leaf node: " << rid.pageNum << " " << rid.slotNum << endl;

    switch(attribute.type) {
        case TypeInt: {
            stack<pair<int, int> > entriesSoFar; //key, offset
            int offset = 0;
            int firstEntryKey = *(int*) pageData;
            int recordsEndOffset = PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData);
            while(offset <= PAGE_SIZE) {
                void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
                int currentOffsetEntryLen;
                readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntry, currentOffsetEntryLen);
                int currentKey = *(int*) currentOffsetEntry;
                if(offset + currentOffsetEntryLen > (PAGE_SIZE/2)) {
                    break;
                } else {
                    entriesSoFar.push(make_pair(currentKey, offset));
                    offset += currentOffsetEntryLen;
                }
                free(currentOffsetEntry);
            }

            void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
            int currentOffsetEntryLen;
            readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntry, currentOffsetEntryLen);
            int currentKey = *(int*) currentOffsetEntry;

            if(currentKey != firstEntryKey) {
                while(!entriesSoFar.empty()) {
                    int offsetTemp = entriesSoFar.top().second;
                    int keyTemp = entriesSoFar.top().first;
                    entriesSoFar.pop();
                    if(keyTemp == currentKey) {
                        continue;
                    } else {
                        int offsetToSplit = offsetTemp + sizeof(int) + sizeof(RID);
                        int lengthToShift = recordsEndOffset - offsetToSplit;
                        memcpy(newPageData, (char*)pageData + offsetToSplit, lengthToShift);
                        setFreeSpace(newPageData, PAGE_SIZE - lengthToShift - PAGE_TYPE_OFFSET);
                        setFreeSpace(pageData, getFreeSpaceFromPage(pageData) + lengthToShift);
                        setPageType(newPageData, LEAF);
                        memset((char*)pageData + offsetToSplit, 0, lengthToShift);
                        return 0;
                    }

                }
            } else {
                offset+=currentOffsetEntryLen;
                while(offset <= PAGE_SIZE) {
                    void* currentOffsetEntryT = calloc(PAGE_SIZE, 1);
                    int currentOffsetEntryLenT;
                    readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntryT, currentOffsetEntryLenT);
                    int currentKeyT = *(int*) currentOffsetEntryT;
                    if(currentKeyT != firstEntryKey) {
                        int offsetToSplit = offset;
                        int lengthToShift = recordsEndOffset - offsetToSplit;
                        memcpy(newPageData, (char*)pageData + offsetToSplit, lengthToShift);
                        setFreeSpace(newPageData, PAGE_SIZE - lengthToShift - PAGE_TYPE_OFFSET);
                        setFreeSpace(pageData, getFreeSpaceFromPage(pageData) + lengthToShift);
                        setPageType(newPageData, LEAF);
                        memset((char*)pageData + offsetToSplit, 0, lengthToShift);
                        return 0;
                    } else {
                        offset += currentOffsetEntryLenT;
                    }
                }
            }
            free(currentOffsetEntry);
            break;
        }
        case TypeReal: {
            stack<pair<float, int> > entriesSoFar; //key, offset
            int offset = 0;
            int firstEntryKey = *(float*) pageData;
            int recordsEndOffset = PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData);
            while(offset <= PAGE_SIZE) {
                void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
                int currentOffsetEntryLen;
                readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntry, currentOffsetEntryLen);
                float currentKey = *(float*) currentOffsetEntry;
                if(offset + currentOffsetEntryLen > (PAGE_SIZE/2)) {
                    break;
                } else {
                    entriesSoFar.push(make_pair(currentKey, offset));
                    offset += currentOffsetEntryLen;
                }
                free(currentOffsetEntry);
            }

            void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
            int currentOffsetEntryLen;
            readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntry, currentOffsetEntryLen);
            float currentKey = *(float*) currentOffsetEntry;

            if(currentKey != firstEntryKey) {
                while(!entriesSoFar.empty()) {
                    int offsetTemp = entriesSoFar.top().second;
                    int keyTemp = entriesSoFar.top().first;
                    entriesSoFar.pop();
                    if(keyTemp == currentKey) {
                        continue;
                    } else {
                        int offsetToSplit = offsetTemp + sizeof(float) + sizeof(RID);
                        int lengthToShift = recordsEndOffset - offsetToSplit;
                        memcpy(newPageData, (char*)pageData + offsetToSplit, lengthToShift);
                        setFreeSpace(newPageData, PAGE_SIZE - lengthToShift - NEXT_PAGE_OFFSET);
                        setFreeSpace(pageData, getFreeSpaceFromPage(pageData) + lengthToShift);
                        setPageType(newPageData, LEAF);
                        memset((char*)pageData + offsetToSplit, 0, lengthToShift);
                        return 0;
                    }

                }
            } else {
                offset+=currentOffsetEntryLen;
                while(offset <= PAGE_SIZE) {
                    void* currentOffsetEntryT = calloc(PAGE_SIZE, 1);
                    int currentOffsetEntryLenT;
                    readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntryT, currentOffsetEntryLenT);
                    float currentKeyT = *(float*) currentOffsetEntryT;
                    if(currentKeyT != firstEntryKey) {
                        int offsetToSplit = offset;
                        int lengthToShift = recordsEndOffset - offsetToSplit;
                        memcpy(newPageData, (char*)pageData + offsetToSplit, lengthToShift);
                        setFreeSpace(newPageData, PAGE_SIZE - lengthToShift - PAGE_TYPE_OFFSET);
                        setFreeSpace(pageData, getFreeSpaceFromPage(pageData) + lengthToShift);
                        setPageType(newPageData, LEAF);
                        memset((char*)pageData + offsetToSplit, 0, lengthToShift);
                        return 0;
                    } else {
                        offset += currentOffsetEntryLenT;
                    }
                }
            }
            free(currentOffsetEntry);
            break;
        }
        case TypeVarChar: {
            stack<tuple<char*, int, int> > entriesSoFar; //key, offset, length
            int offset = 0;
            int firstEntryKeyLength = *(unsigned short*)pageData;
            char* firstEntryKey = (char*) pageData + sizeof(unsigned short);

            int recordsEndOffset = PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData);
            while(offset <= PAGE_SIZE) {
                void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
                int currentOffsetEntryLen;
                readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntry, currentOffsetEntryLen);
                int varcharLength = *(unsigned short*) currentOffsetEntry;
                if(offset + currentOffsetEntryLen > (PAGE_SIZE/2)) {
                    break;
                } else {
                    entriesSoFar.push(make_tuple((char*)pageData + offset + sizeof(unsigned short), offset, varcharLength));
                    offset += currentOffsetEntryLen;
                }
                free(currentOffsetEntry);
            }

            void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
            int currentOffsetEntryLen;
            readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntry, currentOffsetEntryLen);
            int currentEntryKeyLength = *(unsigned short*)currentOffsetEntry;
            char* currentEntryKey = (char*) currentOffsetEntry + sizeof(unsigned short);

            if(strcmp(firstEntryKey, currentEntryKey) != 0) { //TODO: find a way to change this to strncmp
                while(!entriesSoFar.empty()) {
                    int offsetTemp = get<1>(entriesSoFar.top());
                    char* keyTemp = get<0>(entriesSoFar.top());
                    int keyLength = get<2>(entriesSoFar.top());
                    entriesSoFar.pop();
                    if(strcmp(keyTemp, currentEntryKey) == 0) { //TODO: find a way to change this to strncmp
                        continue;
                    } else {
                        int offsetToSplit = offsetTemp + sizeof(unsigned short) + keyLength + sizeof(RID);
                        int lengthToShift = recordsEndOffset - offsetToSplit;
                        memcpy(newPageData, (char*)pageData + offsetToSplit, lengthToShift);
                        setFreeSpace(newPageData, PAGE_SIZE - lengthToShift - PAGE_TYPE_OFFSET);
                        setFreeSpace(pageData, getFreeSpaceFromPage(pageData) + lengthToShift);
                        setPageType(newPageData, LEAF);
                        memset((char*)pageData + offsetToSplit, 0, lengthToShift);
                        return 0;
                    }

                }
            } else {
                offset+=currentOffsetEntryLen;
                while(offset <= PAGE_SIZE) {
                    void* currentOffsetEntryT = calloc(PAGE_SIZE, 1);
                    int currentOffsetEntryLenT;
                    readLeafEntryAtOffset(offset, pageData, attribute, currentOffsetEntryT, currentOffsetEntryLenT);
                    int currentKeyLengthT = *(unsigned short*)currentOffsetEntryT;
                    char* currentKeyT = (char*) currentOffsetEntryT + sizeof(unsigned short);
                    if(strcmp(currentKeyT, firstEntryKey) != 0) { //TODO: find a way to change this to strncmp
                        int offsetToSplit = offset;
                        int lengthToShift = recordsEndOffset - offsetToSplit;
                        memcpy(newPageData, (char*)pageData + offsetToSplit, lengthToShift);
                        setFreeSpace(newPageData, PAGE_SIZE - lengthToShift - PAGE_TYPE_OFFSET);
                        setFreeSpace(pageData, getFreeSpaceFromPage(pageData) + lengthToShift);
                        setPageType(newPageData, LEAF);
                        memset((char*)pageData + offsetToSplit, 0, lengthToShift);
                        return 0;
                    } else {
                        offset += currentOffsetEntryLenT;
                    }
                }
            }
            free(currentOffsetEntry);
            break;
        }
        default:
            cout << "[ERROR]Invalid attribute type in index" << endl;
            break;
    }
    return 0;
}

RC IndexManager::squeezeEntryIntoNonLeaf(void* pageData, const Attribute &attribute, const void* key, const PageNum pointerPageNum) {
    int offset = 0;
    bool flag = true;
    int endOfRecordsOffset = getEndOfRecordOffsetFromPage(pageData);
    offset += sizeof(PageNum);
    while(offset <= PAGE_SIZE && flag) {
//        void* currentOffsetEntry = calloc(PAGE_SIZE, 1);
        switch(attribute.type) {
            case TypeInt: {
                if(offset >= endOfRecordsOffset) {
                    *(int*)((char*)pageData + offset) = *(int*)key;
                    *(PageNum*)((char*)pageData + offset + sizeof(int)) = pointerPageNum;
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - (sizeof(int) + sizeof(PageNum)));
                    return 0;
                }
                int currentKey = *(int *) ((char*)pageData + offset);
                if (*(int *) key < currentKey) {
                    int lengthToShift = endOfRecordsOffset - offset;
                    if(lengthToShift > 0)
                        memmove((char *) pageData + offset + sizeof(int) + sizeof(PageNum), (char *) pageData + offset, lengthToShift);

                    *(int*)((char*)pageData + offset) = *(int*)key;
                    *(PageNum*)((char*)pageData + offset + sizeof(int)) = pointerPageNum;
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - (sizeof(int) + sizeof(PageNum)));
                    return 0;
                } else {
                    offset += (sizeof(int) + sizeof(PageNum));
                }
                break;
            }
            case TypeReal: {
                if(offset >= endOfRecordsOffset) {
                    *(float*)((char*)pageData + offset) = *(float*)key;
                    *(PageNum*)((char*)pageData + offset + sizeof(int)) = pointerPageNum;
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - (sizeof(int) + sizeof(PageNum)));
                    return 0;
                }
                float currentKey = *(float *) ((char*)pageData + offset);
                if (*(float *) key < currentKey) {
                    int endOfRecordsOffset = PAGE_SIZE - PAGE_TYPE_OFFSET - getFreeSpaceFromPage(pageData);
                    int lengthToShift = endOfRecordsOffset - offset;
                    if(lengthToShift > 0)
                        memmove((char *) pageData + offset + sizeof(float) + sizeof(PageNum), (char *) pageData + offset, lengthToShift);
                    *(float*)((char*)pageData + offset) = *(float*)key;
                    *(PageNum*)((char*)pageData + offset + sizeof(int)) = pointerPageNum;
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - (sizeof(float) + sizeof(PageNum)));
                    return 0;
                } else {
                    offset += (sizeof(float) + sizeof(PageNum));
                }
                break;
            }
            case TypeVarChar: {
                if(offset >= endOfRecordsOffset) {
                    int varCharLen = *(unsigned short*)key;
                    memcpy((char *) pageData + offset, key, sizeof(unsigned short) + varCharLen);
                    *(PageNum*)((char*)pageData + sizeof(unsigned short) + varCharLen) = pointerPageNum;
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - (sizeof(unsigned short) + varCharLen +
                                                                             sizeof(PageNum)));
                    return 0;
                }
                int varcharLength = *(unsigned short*) ((char*)pageData + offset);
                char* relevantCurrentIndexString = (char*) calloc(PAGE_SIZE, 1);
                memcpy(relevantCurrentIndexString, (char*)pageData+offset + sizeof(unsigned short), varcharLength);
                int insertKeyLength = *(unsigned short*) key;
                char* relevantInsertKeyString = (char*) calloc(PAGE_SIZE, 1);
                memcpy(relevantInsertKeyString, (char*)key + sizeof(unsigned short), insertKeyLength);
                if(strcmp(relevantInsertKeyString, relevantCurrentIndexString) < 0) {
                    int lengthToShift = endOfRecordsOffset - offset;
                    if(lengthToShift > 0)
                        memmove((char *) pageData + offset + sizeof(unsigned short) + insertKeyLength + sizeof(PageNum), (char *) pageData + offset, lengthToShift);
                    memcpy((char *) pageData + offset, key, sizeof(unsigned short) + insertKeyLength);
                    *(PageNum*)((char*)pageData + offset + sizeof(unsigned short) + insertKeyLength) = pointerPageNum;
                    setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - (sizeof(unsigned short)+insertKeyLength+
                                                                             sizeof(PageNum)));
                    return 0;
                } else {
                    offset += sizeof(unsigned short) + varcharLength + sizeof(PageNum);
                }
                break;
            }
            default:
                cout << "[ERROR]Invalid attribute type in index" << endl;
                break;
        }
    }
    return -1;
}

PageNum IndexManager::getRightInsertPage(void* pageData, const Attribute &attribute, const void* key) {
    int freeSpace = getFreeSpaceFromPage(pageData);
    int endOfRecordsOffset = getEndOfRecordOffsetFromPage(pageData);
    int offset = 0;
    offset += sizeof(PageNum);
    while(offset < endOfRecordsOffset) {
        switch(attribute.type) {
            case TypeInt: {
                int key_i = *(int*)((char*)pageData + offset);
                PageNum p_i = *(PageNum*)((char*)pageData + offset + sizeof(int));
                if(offset + sizeof(int) + sizeof(PageNum) >= endOfRecordsOffset) {
                    if(*(int*)key >= key_i) {
                        return p_i;
                    }
                } else {
                    int key_iplus1 = *(int*)((char*)pageData + offset + sizeof(int) + sizeof(PageNum));
                    if(*(int*)key >= key_i && *(int*)key < key_iplus1) {
                        return p_i;
                    } else {
                        offset += (sizeof(int) + sizeof(PageNum));
                    }

                }
                break;
            }
            case TypeReal: {
                float key_i = *(float*)((char*)pageData + offset);
                PageNum p_i = *(PageNum*)((char*)pageData + offset + sizeof(float));
                if(offset + sizeof(float) + sizeof(PageNum) >= endOfRecordsOffset) {
                    if(*(float*)key >= key_i) {
                        return p_i;
                    }
                } else {
                    float key_iplus1 = *(float*)((char*)pageData + offset + sizeof(float) + sizeof(PageNum));
                    if(*(float*)key >= key_i && *(float*)key < key_iplus1) {
                        return p_i;
                    } else {
                        offset += (sizeof(float) + sizeof(PageNum));
                    }

                }
                break;
            }
            case TypeVarChar: {
                int varcharLength = *(unsigned short*)((char*)key);
                char* key_string = (char*) calloc(varcharLength, 1);
                memcpy(key_string, (char*)key + sizeof(unsigned short), varcharLength);

                int varcharLength_i = *(unsigned short*)((char*)pageData + offset);
                char* key_i = (char*) calloc(varcharLength_i, 1);
                memcpy(key_i, (char*)pageData + offset + sizeof(unsigned short), varcharLength_i);
                PageNum p_i = *(PageNum*)((char*)pageData + offset + sizeof(unsigned short) + varcharLength_i);
                if((offset + sizeof(unsigned short) + varcharLength_i + sizeof(PageNum)) >= endOfRecordsOffset) {
                    if(strcmp(key_string, key_i) >= 0) {
                        return p_i;
                    }
                } else {
                    int varcharLength_iplus1 = *(unsigned short*)((char*)pageData + offset + sizeof(unsigned short) + varcharLength_i +
                                                                  sizeof(PageNum));
                    char* key_iplus1 = (char*) calloc(varcharLength_iplus1, 1);
                    memcpy(key_iplus1, (char*)pageData + offset + sizeof(unsigned short) + varcharLength_i + sizeof(PageNum) +
                                       sizeof(unsigned short), varcharLength_iplus1);

                    if((strcmp(key_string, key_i) >= 0) && (strcmp(key_string, key_iplus1) < 0)) {
                        return p_i;
                    } else {
                        offset += (sizeof(unsigned short) + varcharLength_i + sizeof(PageNum));
                    }
                }
                free(key_i);
                break;
            }
            default:
                cout << "[ERROR]Invalid attribute type in index" << endl;
                break;
        }
    }
}

RC IndexManager::splitNonLeafNode(void* pageData, void* newPageData, const void* key, const Attribute &attribute, const PageNum pointerPageNum, int &location) {
    int offset = 0;
    offset += sizeof(PageNum);
    int endOfRecordsOffset = getEndOfRecordOffsetFromPage(pageData);
    while(offset < endOfRecordsOffset) {
        int entryLength;
        switch(attribute.type) {
            case TypeInt: {
                entryLength = sizeof(int) + sizeof(PageNum);
                break;
            }
            case TypeReal: {
                entryLength = sizeof(float) + sizeof(PageNum);
                break;
            }
            case TypeVarChar: {
                int varcharLength = *(unsigned short*)((char*)pageData + offset);
                entryLength = sizeof(unsigned int) + varcharLength + sizeof(PageNum);
                break;
            }
            default: {
                cout << "Invalid attribute type" << endl;
                break;
            }
        }
        if(offset + entryLength > PAGE_SIZE/2) {

            switch(attribute.type) {
                case TypeInt: {
                    if (*(int *) key < *(int *) ((char *) pageData + offset)) {
                        location = 0;
                    } else {
                        location = 1;
                    }
                    break;
                }
                case TypeReal: {

                    if (*(float *) key < *(float *) ((char *) pageData + offset)) {
                        location = 0;
                    } else {
                        location = 1;
                    }
                    break;
                }
                case TypeVarChar: {
                    int varcharLength = *(unsigned short*)((char*)pageData + offset);
                    char* current_s = (char*)calloc(varcharLength, 1);
                    int varcharLength_key = *(int*)key;
                    char* key_s = (char*)calloc(varcharLength_key, 1);
                    if(strcmp(key_s, current_s) < 0) {
                        location = 0;
                    } else {
                        location = 1;
                    }
                    free(current_s);
                    free(key_s);
                    break;
                }
                default:
                    cout << "Invalid attribute type" << endl;
                    break;
            }

            int offsetToSplit = offset;
            int lengthToShift = endOfRecordsOffset - offsetToSplit;
            memcpy(newPageData, (char*)pageData + offsetToSplit, lengthToShift);
            setFreeSpace(newPageData, PAGE_SIZE - lengthToShift - PAGE_TYPE_OFFSET);
            setFreeSpace(pageData, getFreeSpaceFromPage(pageData) + lengthToShift);
            setPageType(newPageData, NON_LEAF);
            memset((char*)pageData + offsetToSplit, 0, lengthToShift);
            return 0;
        } else {
            offset += entryLength;
        }
    }
    cout << "Reached the end of records in splitNonLeafNode. Something's not right here!"<<endl;
}

/**
 *
 * @param ixfileHandle
 * @param currPageNum
 * @param attribute
 * @param key
 * @param rid
 * @param newChildPageNum
 * @return
 * This is the main recursive demon for insertion. Thread carefully cause it bites
 */
RC IndexManager::insertIntoTree(IXFileHandle &ixfileHandle, PageNum currPageNum, const Attribute &attribute, const void* key, const RID &rid, PageNum &newChildPageNum, void* splitKey) {
    void* pageData = calloc(PAGE_SIZE, 1);
    int result = ixfileHandle.readPage(currPageNum, pageData);
    if(result != 0)
        return result;
    int pageType = getPageTypeFromPage(pageData);
    int freeSpace = getFreeSpaceFromPage(pageData);

    if(pageType == NON_LEAF) {
        PageNum p0 = *(PageNum*)pageData;
        int key1 = *(int *)((char*)pageData + sizeof(PageNum));
        PageNum  pi;
        bool leftMostCondition;
        switch(attribute.type) {
            case TypeInt: {
                leftMostCondition = (*(int*)key < *(int *)((char*)pageData + sizeof(PageNum))) ? true: false;
                break;
            }
            case TypeReal: {
                leftMostCondition = (*(float*)key < *(float *)((char*)pageData + sizeof(PageNum))) ? true: false;
                break;
            }
            case TypeVarChar: {
                int key_length = *(int*)key;
                int key1_length = *(unsigned short*)((char*)pageData + sizeof(PageNum));
                char* key_s = (char*) calloc(PAGE_SIZE, 1);
                char* key1_s = (char*) calloc(PAGE_SIZE, 1);
                memcpy(key_s, (char*)key + sizeof(int), key_length);
                memcpy(key1_s, (char*)pageData + sizeof(PageNum) + sizeof(unsigned short), key1_length);
                leftMostCondition = (strcmp(key_s, key1_s) < 0) ? true : false;
                free(key_s);
                free(key1_s);
                break;
            }
            default:
                break;
        }
        if(leftMostCondition) {
            pi = p0;
        } else {
            pi = getRightInsertPage(pageData, attribute, key);
        }
        result = insertIntoTree(ixfileHandle, pi, attribute, key, rid, newChildPageNum, splitKey);



        if(newChildPageNum != -1) {
            int spaceNeeded;
            switch(attribute.type) {
                case TypeInt: {
                    spaceNeeded = sizeof(int) + sizeof(PageNum);
                    break;
                }
                case TypeReal: {
                    spaceNeeded = sizeof(float) + sizeof(PageNum);
                    break;
                }
                case TypeVarChar: {
                    int key_length = *(int*)key;
                    spaceNeeded = sizeof(int) + key_length + sizeof(PageNum);
                    break;
                }
                default:
                    break;
            }
            if(freeSpace >= spaceNeeded) {
                squeezeEntryIntoNonLeaf(pageData, attribute, splitKey, newChildPageNum);
                newChildPageNum = -1;
                memset(splitKey, 0, PAGE_SIZE);
                return 0;
            } else {
                cout << "Split of a non-leaf node triggered!" << endl;
                void* newPageData = calloc(PAGE_SIZE, 1);
                int location;
                splitNonLeafNode(pageData, newPageData, key, attribute, newChildPageNum, location);
                if(location == 0) {
                    squeezeEntryIntoNonLeaf(pageData, attribute, splitKey, newChildPageNum);
                } else {
                    squeezeEntryIntoNonLeaf(newPageData, attribute, splitKey, newChildPageNum);
                }
                result = ixfileHandle.appendPage(newPageData);
                if(result != 0)
                    cout << "[ERROR]Issue in append page during non-leaf split" << endl;
                result = ixfileHandle.writePage(currPageNum, pageData);
                if(result != 0)
                    cout << "[ERROR]Issue in write page during non-leaf split" << endl;
                PageNum newlyAddedPageNum = ixfileHandle.getPersistedAppendCounter() - 1;
                newChildPageNum = newlyAddedPageNum;
                *(int*)splitKey = *(int*)newPageData;

                if(indexRootNodeMap[ixfileHandle.fileName] == currPageNum) {
                    cout << "Root non leaf node just split"<<endl;
                    void* newRootNode = calloc(PAGE_SIZE, 1);
                    *(PageNum *)newRootNode = currPageNum;
                    int sizeOfInternalKey;
                    switch(attribute.type) {
                        case TypeInt: {
                            *(int*)((char*)newRootNode + sizeof(int)) = *(int*)newPageData;
                            sizeOfInternalKey = 4;
                            *(PageNum *)((char*)newRootNode + sizeof(PageNum) + sizeof(int)) = newlyAddedPageNum;
                            break;
                        }
                        case TypeReal: {
                            *(float*)((char*)newRootNode + sizeof(PageNum)) = *(float*)newPageData;
                            sizeOfInternalKey = 4;
                            *(int*)((char*)newRootNode + sizeof(PageNum) + sizeof(float)) = newlyAddedPageNum;
                            break;
                        }
                        case TypeVarChar: {
                            int varCharLength = *(unsigned short*)newPageData;
                            memcpy((char*)newRootNode + sizeof(PageNum), newPageData, sizeof(unsigned short) + varCharLength);
                            sizeOfInternalKey = sizeof(unsigned short) + varCharLength;
                            *(int*)((char*)newRootNode + sizeof(PageNum) + sizeof(int)) = newlyAddedPageNum;
                            break;
                        }
                        default:
                            break;
                    }
                    setPageType(newRootNode, NON_LEAF);
                    setFreeSpace(newRootNode, PAGE_SIZE - PAGE_TYPE_OFFSET - (sizeof(PageNum) + sizeOfInternalKey + sizeof(PageNum)));

                    result = ixfileHandle.appendPage(newRootNode);
                    int newRootNodePageNum = ixfileHandle.getPersistedAppendCounter() - 1;
                    indexRootNodeMap[ixfileHandle.fileName] = newRootNodePageNum;
                    persistIndexRootNodeMap();
                    free(newRootNode);

                }

                    free(newPageData);
            }
        } else {
            return 0;
        }
    } else if(pageType == LEAF) {
        void* entry = calloc(PAGE_SIZE, 1);
        int entryLen;
        createLeafEntry(attribute, key, rid, entry, entryLen);
        if(freeSpace >= entryLen) {
            if(freeSpace == (PAGE_SIZE - 4)) {
                cout<<"First entry: "<< (char*)entry+2 <<endl;
                memcpy(pageData, entry, entryLen);
                setFreeSpace(pageData, getFreeSpaceFromPage(pageData) - entryLen);
            } else {
                result = squeezeEntryIntoLeaf(pageData, attribute, entry, entryLen, key, rid);
//                cout <<"Just checking length " << *(unsigned short*)entry << endl;
                if(result != 0) {
                    cout << "[ERROR]Squeeze entry shows unexpected behavior" << endl;
                }
            }

            ixfileHandle.writePage(currPageNum, pageData);
            newChildPageNum = -1;
        } else {
            void* newPageData = calloc(PAGE_SIZE, 1);
            splitLeafNode(pageData, newPageData, attribute, entry, entryLen, key, rid);
            squeezeEntryIntoLeaf(newPageData, attribute, entry, entryLen, key, rid);
            result = ixfileHandle.appendPage(newPageData);
            if(result != 0)
                cout << "[ERROR]Issue in append page during leaf split" << endl;
            result = ixfileHandle.writePage(currPageNum, pageData);
            if(result != 0)
                cout << "[ERROR]Issue in append page during leaf split" << endl;
            PageNum newlyAddedPageNum = ixfileHandle.getPersistedAppendCounter() - 1;
            setNextSiblingPage(pageData, newlyAddedPageNum);
            newChildPageNum = newlyAddedPageNum;
            switch(attribute.type) {
                case TypeInt: {
                    *(int*)splitKey = *(int*)newPageData;
                    break;
                }
                case TypeReal: {
                    *(float*)splitKey = *(float*)newPageData;
                    break;
                }
                case TypeVarChar: {
                    unsigned short varcharLen = *(unsigned short*)newPageData;
                    *(unsigned short*)splitKey = varcharLen;
                    memcpy((char*)splitKey + sizeof(unsigned short), (char*)newPageData + sizeof(unsigned short), varcharLen);
                    break;
                }
                default:
                    break;
            }

            if(indexRootNodeMap[ixfileHandle.fileName] == currPageNum) {
                cout << "The root leaf node just split" << endl;
                void* newRootNode = calloc(PAGE_SIZE, 1);
                *(PageNum *)newRootNode = currPageNum;
                int sizeOfInternalKey;
                switch(attribute.type) {
                    case TypeInt: {
                        *(int*)((char*)newRootNode + sizeof(int)) = *(int*)newPageData;
                        sizeOfInternalKey = 4;
                        *(PageNum *)((char*)newRootNode + sizeof(PageNum) + sizeof(int)) = newlyAddedPageNum;
                        break;
                    }
                    case TypeReal: {
                        *(float*)((char*)newRootNode + sizeof(PageNum)) = *(float*)newPageData;
                        sizeOfInternalKey = 4;
                        *(int*)((char*)newRootNode + sizeof(PageNum) + sizeof(float)) = newlyAddedPageNum;
                        break;
                    }
                    case TypeVarChar: {
                        int varCharLength = *(unsigned short*)newPageData;
                        memcpy((char*)newRootNode + sizeof(PageNum), newPageData, sizeof(unsigned short) + varCharLength);
                        sizeOfInternalKey = sizeof(unsigned short) + varCharLength;
                        *(int*)((char*)newRootNode + sizeof(PageNum) + sizeof(int)) = newlyAddedPageNum;
                        break;
                    }
                    default:
                        break;
                }
                setPageType(newRootNode, NON_LEAF);
                setFreeSpace(newRootNode, PAGE_SIZE - PAGE_TYPE_OFFSET - (sizeof(PageNum) + sizeOfInternalKey + sizeof(PageNum)));

                result = ixfileHandle.appendPage(newRootNode);
                int newRootNodePageNum = ixfileHandle.getPersistedAppendCounter() - 1;
                indexRootNodeMap[ixfileHandle.fileName] = newRootNodePageNum;
                persistIndexRootNodeMap();
                free(newRootNode);
            }

            free(newPageData);
        }
        free(entry);

    } else {
        cout << "Unrecognised page type!" << endl;
    }
    free(pageData);
    return 0;
}

RC IndexManager::insertEntry(IXFileHandle &ixfileHandle, const Attribute &attribute, const void *key, const RID &rid)
{
    int result;

    map<string, PageNum>::iterator it = indexRootNodeMap.find(ixfileHandle.fileName);
//    cout << "Printing out map values of size: " <<  indexRootNodeMap.size() << endl;
//
//    for (auto iter = indexRootNodeMap.begin(); iter != indexRootNodeMap.end(); iter++)
//    {
//        cout << "Key: " << iter->first << endl << "Values: "<< iter->second << endl;
//    }
    bool truth = it == indexRootNodeMap.end();
    if(truth) {
        void* firstPageData = calloc(PAGE_SIZE, 1);
        unsigned short freeSpace = PAGE_SIZE - NEXT_PAGE_OFFSET;
        unsigned short pageType = LEAF; //0 implies leaf node
        memcpy((unsigned short*)((char*)firstPageData+PAGE_SIZE-PAGE_TYPE_OFFSET), &pageType, sizeof(unsigned short));
        memcpy((unsigned short*)((char*)firstPageData+PAGE_SIZE-FREE_SPACE_OFFSET), &freeSpace, sizeof(unsigned short));
        result = ixfileHandle.appendPage(firstPageData);
        free(firstPageData);
        int pageNumAdded = ixfileHandle.getPersistedAppendCounter() - 1;
        indexRootNodeMap[ixfileHandle.fileName] = pageNumAdded;
        cout << "File name is: " << ixfileHandle.fileName<<endl;
        result = persistIndexRootNodeMap();

    }

    PageNum newChildEntry = -1;
    void* splitEntry = calloc(PAGE_SIZE, 1);
    result = insertIntoTree(ixfileHandle, indexRootNodeMap[ixfileHandle.fileName], attribute, key, rid, newChildEntry, splitEntry);
    free(splitEntry);
    return result;
}

RC IndexManager::deleteEntry(IXFileHandle &ixfileHandle, const Attribute &attribute, const void *key, const RID &rid)
{
	PageNum root = indexRootNodeMap[ixfileHandle.fileName];
	int bytesToRead;
	if(attribute.type == TypeInt || attribute.type == TypeReal){
		cout << "Its favorable type" << endl;
		bytesToRead = 4;
	}
	void* pageData = calloc(PAGE_SIZE, 1);
	ixfileHandle.readPage(root, pageData);
	cout << "read the page" << endl;
	int leafPage = findLeaf(ixfileHandle, pageData, root, attribute, key);
	if(leafPage == -1) {
		// doesnt exist, return something or fill up the data members of ixscan
	}
	int lastOffset = getEndOfRecordOffsetFromPage(pageData);
	bool foundKey = false;
	bool foundRID = false;
	int offset = 0;
	int start = 0;
	int entryLen = 0;
	while(offset < lastOffset && !foundKey && !foundRID){
		if(attribute.type == TypeInt) {
			if(*(int *) key == getIntValueAtOffset(pageData, offset)){
				foundKey = true;
			}
			offset += 4;
		} else if(attribute.type == TypeReal) {
			if(*(float *) key == getRealValueAtOffset(pageData, offset)){
				foundKey = true;
			}
			offset += 4;
		} else if(attribute.type == TypeVarChar) {
			void* len = calloc(4, 1);
			memcpy(len, (char*) key, 4);
			int keyLen = *(int *) len;
			void* currKeyLen = calloc(2, 1);
			memcpy(currKeyLen, (char*) pageData + offset, 2);
			int length = *(unsigned short *) currKeyLen; //getIntValueAtOffset(pageData, offset); // do short
			offset += 2;
			if(keyLen == length) {
				void* currKey = calloc(length, 1);
				memcpy(currKey, (char*) pageData + offset, length);
				string currKeyStr((char*) currKey, length);
				void* keyStrPtr = calloc(length, 1);
				memcpy(keyStrPtr, (char*) key + 4, length);

				string keyStr((char*) keyStrPtr, length);
				if(currKeyStr.compare(keyStr) == 0) {
					foundKey = true;
				}
			}
			offset += length;
		}

		if(foundKey) {
			int page = getIntValueAtOffset(pageData, offset);
			int slot = getIntValueAtOffset(pageData, offset + 4);
			if(page == rid.pageNum && slot == rid.slotNum) {
				foundRID = true;
			} else {
				foundKey = false;
				foundRID = false;
			}
		}
		offset += 8;
		entryLen = offset - start;
		start = offset;
	}

	if(foundKey && foundRID) {
		start -= entryLen;
		int toCopy = getEndOfRecordOffsetFromPage(pageData) - offset;
		memmove((char*) pageData + start, (char *)pageData + offset, toCopy);
		unsigned short freeSpace = getFreeSpaceFromPage(pageData);
		freeSpace += entryLen;
		setFreeSpace(pageData, freeSpace);
		ixfileHandle.writePage(leafPage, pageData);
		return 0;
	}

    return -1;
}



void IndexManager::printBTreeRecursively(IXFileHandle &ixfileHandle, const Attribute &attribute, int pageNum, int depth) const {
    int result;
    void* pageData = calloc(PAGE_SIZE, 1);
    result = ixfileHandle.readPage(pageNum, pageData);
    int pageType = *(unsigned short*)((char*)pageData + PAGE_SIZE - PAGE_TYPE_OFFSET);

    if(pageType == LEAF) {
        int d = 0;
        while(d < depth){
        		cout << "\t";
        		d++;
        }
    		cout << "{\"keys\":[";
        int freeSpace = *(unsigned short*)((char*)pageData + PAGE_SIZE - FREE_SPACE_OFFSET);
        int endIndex = PAGE_SIZE - NEXT_PAGE_OFFSET - freeSpace;
        int offset = 0;
//        typedef std::map<string, vector<tuple<int, int> > > keysMap;

        typedef std::vector<tuple<int, int> >  ridList;
        typedef std::map<string, ridList> keysMap;
        keysMap kMap;

        switch(attribute.type) {
            case TypeInt: {
                while(offset < endIndex) {
                    if(offset > 0)
                        cout << ",";
                    int key = *(int*)((char*)pageData + offset);
                    offset += sizeof(int);
                    RID rid = *(RID*)((char*)pageData + offset);
                    offset += sizeof(RID);
                    cout << "\"" + to_string(key) + string(":[(") + to_string(rid.pageNum) + string(",") + to_string(rid.slotNum) + string(")]\"");
//                    keysMap::iterator it = kMap.find(to_string(key));
//                    if(it != kMap.end()) {
//                        kMap[to_string(key)].push_back(tuple<int, int>(rid.pageNum, rid.slotNum));
//                    } else {
//                        ridList newList;
//                        newList.push_back(tuple<int, int>(rid.pageNum, rid.slotNum));
//                        kMap[to_string(key)] = newList;
//                    }
                }
                break;
            }
            case TypeReal: {
                while(offset < endIndex) {
                    if(offset > 0)
                        cout << ",";
                    float key = *(float*)((char*)pageData + offset);
                    offset += sizeof(float);
                    RID rid = *(RID*)((char*)pageData + offset);
                    offset += sizeof(RID);
                    cout << "\"" << key << string(":[(") + to_string(rid.pageNum) + string(",") + to_string(rid.slotNum) + string(")]\"");
                }
                break;
            }
            case TypeVarChar: {
                while(offset < endIndex) {
                    if(offset > 0)
                        cout << ",";
                    unsigned short varCharLength = *(unsigned short*)((char*)pageData + offset);
                    offset+= sizeof(unsigned short);
                    char* varCharData = (char*) calloc(PAGE_SIZE, 1);
                    memcpy(varCharData, (char*)pageData + offset, varCharLength);
//                    cout << "\nWhile printing varchar lengths are: " << varCharLength << endl;
                    offset += varCharLength;
                    RID rid = *(RID*)((char*)pageData + offset);
                    offset += sizeof(RID);
                    cout << string("\"") + varCharData + ":[(" + to_string(rid.pageNum) + "," + to_string(rid.slotNum) + ")]\"";
                    free(varCharData);
                }
                break;
            }
            default:
                cout << "[ERROR]Invalid attribute type in index" << endl;
                break;
        }
        cout<<"]}";

    } else {
        //TODO: in case of non leaf
		int d = 0;
		while(d < depth){
			cout << "\t";
			d++;
		}
    		cout << "{" << endl;
    		d = 0;
		while(d < depth){
			cout << "\t";
			d++;
		}
    		cout << "\"keys\"" << ":[";
    		int offset = 0;
		int freeSpace = *(unsigned short*)((char*)pageData + PAGE_SIZE - FREE_SPACE_OFFSET);
		int endIndex = PAGE_SIZE - PAGE_TYPE_OFFSET - freeSpace;
		vector<unsigned int> pages;
		int toggle = 0;
		while(offset < endIndex){
			if(toggle == 0) {
				void* page = calloc(4, 1);
				memcpy(page, (char*)pageData + offset, sizeof(unsigned int));
				pages.push_back(*(unsigned int *) page);
				offset += 4;
				toggle = 1;
			} else {
				if(attribute.type == TypeInt) {
					void *key = calloc(4, 1);
					memcpy(key, (char*)pageData + offset, sizeof(int));
					if(offset != 4) {
						cout << ",";
					}
					cout <<"\"" << *(int *) key << "\"";
					offset += 4;
				} else if(attribute.type == TypeReal) {
					void *key = calloc(4, 1);
					memcpy(key, (char*)pageData + offset, sizeof(float));
					if(offset != 4) {
						cout << ",";
					}
					cout <<"\"" << *(float *) key << "\"";
					offset += 4;
				} else if(attribute.type == TypeVarChar) {
					void *length = calloc(2, 1);
					memcpy(length, (char*)pageData + offset, sizeof(unsigned short));
					offset += sizeof(unsigned short);
					void *key = calloc(sizeof(*(int*) length), 1);
					memcpy(key, (char*)pageData + offset, *(int *) length);
					cout <<"\"" << (char *) key << "\"";
					offset += *(unsigned short *) length;
				}
				toggle = 0;
			}
		}
		cout << "]," << endl;
		d = 0;
		while(d < depth){
			cout << "\t";
			d++;
		}
		cout << "\"children\"" << ": [" << endl;

		int count = 0;
		while(count < pages.size()) {
			if(count != 0) {
				cout << "," << endl;
			}
			printBTreeRecursively(ixfileHandle, attribute, pages[count], depth + 1);
//			cout << endl;
			count++;
		}
		d = 0;
		while(d < depth){
			cout << "\t";
			d++;
		}
		cout << endl;
		cout << "]" << endl << "}" << endl;
        cout << "Printing stuck here!" << endl;
    }
}

void IndexManager::printBtree(IXFileHandle &ixfileHandle, const Attribute &attribute) const {
    string s = ixfileHandle.fileName;
    int rootPageNum = indexRootNodeMap.at(s);
    int depth = 0;
    printBTreeRecursively(ixfileHandle, attribute, rootPageNum, depth);
    cout << endl;
}

IX_ScanIterator::IX_ScanIterator()
{
}

IX_ScanIterator::~IX_ScanIterator()
{
}

RC IndexManager::scan(IXFileHandle &ixfileHandle,
                      const Attribute &attribute,
                      const void      *lowKey,
                      const void      *highKey,
                      bool			lowKeyInclusive,
                      bool        	highKeyInclusive,
                      IX_ScanIterator &ix_ScanIterator)
{

	ix_ScanIterator.ixfileHandle = &ixfileHandle;
	ix_ScanIterator.attribute = attribute;
	if(attribute.type == TypeInt || attribute.type == TypeReal) {
		ix_ScanIterator.lowKey = calloc(4, 1);
		memcpy(ix_ScanIterator.lowKey, lowKey, 4);
		ix_ScanIterator.highKey = calloc(4, 1);
		memcpy(ix_ScanIterator.highKey, highKey, 4);

	} else {
		int len1 = *(int*)lowKey;
		ix_ScanIterator.lowKey = calloc(len1, 1);
		memcpy(ix_ScanIterator.lowKey, (char*)lowKey + 4, len1);
		int len2 = *(int*)highKey;
		ix_ScanIterator.highKey = calloc(len2, 1);
		memcpy(ix_ScanIterator.highKey, (char*)highKey + 4, len2);
	}
	float low = *(float *) ix_ScanIterator.lowKey;
	cout << "Low: " << low << endl;

	ix_ScanIterator.lowKeyInclusive = lowKeyInclusive;
	ix_ScanIterator.highKeyInclusive = highKeyInclusive;
	ix_ScanIterator.indexManager = IndexManager::instance();
	ix_ScanIterator.scanPageData = calloc(PAGE_SIZE, 1);

	PageNum root = indexRootNodeMap[ixfileHandle.fileName];
	int bytesToRead;
	if(attribute.type == TypeInt || attribute.type == TypeReal){
		cout << "Its favorable type" << endl;
		bytesToRead = 4;
	}
//	void* pageData = calloc(PAGE_SIZE, 1);
	ixfileHandle.readPage(root, ix_ScanIterator.scanPageData);
	cout << "root page: " << root << endl;
	cout << "read the page" << endl;
	int leafPage = findLeaf(ixfileHandle, ix_ScanIterator.scanPageData, root, attribute, ix_ScanIterator.lowKey);
	if(leafPage == -1) {
		cout << "weirdddd" << endl;
		// doesnt exist, return something or fill up the data members of ixscan
	}
	ix_ScanIterator.leafPageNum = leafPage;
	cout << "Leaf Page: " << leafPage << endl;
	// Scan through the leaf to find the first one
	int offset = 0;
	int lastOffset = getEndOfRecordOffsetFromPage(ix_ScanIterator.scanPageData);
	cout << "Last off set: " << lastOffset << endl;
	// THIS IS FOR INT
	float key = getRealValueAtOffset(ix_ScanIterator.scanPageData, offset);
//	float low2 = *(float *) ix_ScanIterator.lowKey;
	cout << "Low2: " << low << endl;
	cout << "First key: " << key << endl;

	while(offset < lastOffset && key < low){
		offset += 12;
		key = getRealValueAtOffset(ix_ScanIterator.scanPageData, offset);
	}
	cout << "\n";
	cout << "After while loop.." << endl;
	cout << "Offset:" << offset << endl;
	cout << "key: " << key << endl;
	cout << "\n";
	if(offset == lastOffset) {
		// Not found
	}
	ix_ScanIterator.scanOffset = offset;
	if(key >= low) {
		if(key == low && !lowKeyInclusive) {
			while(key == low){
				offset += 12;
				key = getRealValueAtOffset(ix_ScanIterator.scanPageData, offset);
			}
			ix_ScanIterator.scanOffset = offset;
//			key = getRealValueAtOffset(pageData, offset);
			if(offset >= lastOffset) {
				ix_ScanIterator.end = true;
				//no need of this, can just mention something otherwise
			}
		}
	}
	cout << endl;
	cout << "Finally: " << endl;
	cout << "Offset:" << offset << endl;
	cout << "key: " << key << endl;
	cout << "\n";

	return 0;
}

int IndexManager::findLeaf(IXFileHandle &ixfileHandle, void* pageData, PageNum currPageNum, const Attribute &attribute, const void* lowKey){
	cout << "In find leaf" << endl;
	if(getPageTypeFromPage(pageData) == LEAF){
		cout << "returning: " << currPageNum << endl;
		return currPageNum;
	}
	int offset = 4;
	while(offset < getEndOfRecordOffsetFromPage(pageData)){
		if(attribute.type == TypeInt) {
			int key = getIntValueAtOffset(pageData, offset);
			int low = *(int *) lowKey;
			bool found = false;
			if(key == low) {
				offset += 4;
				found = true;
			} else if(low < key) {
				offset -= 4;
				found = true;
			}
			if(found) {
				PageNum nextPageNum = getIntValueAtOffset(pageData, offset);
				ixfileHandle.readPage(nextPageNum, pageData);
				return findLeaf(ixfileHandle, pageData, nextPageNum, attribute, lowKey);
			}
			offset += 8;
		} else if(attribute.type == TypeReal) {
			cout << "in real , cur pn: " << currPageNum << endl;
			float key = getRealValueAtOffset(pageData, offset);
			cout << "key here: " <<  key << endl;
			float low = *(float *) lowKey;
			bool found = false;
			if(key == low) {
				cout << "key == low" << endl;
				offset += 4;
				found = true;
			} else if(low < key) {
				cout << "low, key: ";
				cout << low << "," << key << endl;
				cout << "low < key" << endl;
				offset -= 4;
				found = true;
			}
			if(found) {
				PageNum nextPageNum = getIntValueAtOffset(pageData, offset);
				ixfileHandle.readPage(nextPageNum, pageData);
				return findLeaf(ixfileHandle, pageData, nextPageNum, attribute, lowKey);
			}
			offset += 8;
			cout << "after 8+ offset: " << offset << endl;
		}
		//TODO: Varchar
	}
	if(offset >= getEndOfRecordOffsetFromPage(pageData)) {
		PageNum nextPageNum = getIntValueAtOffset(pageData, offset - 4);
		ixfileHandle.readPage(nextPageNum, pageData);
		return findLeaf(ixfileHandle, pageData, nextPageNum, attribute, lowKey);
	}
	return -1;
}

float IndexManager::getRealValueAtOffset(void *pageRecord, int offset) {
    void *verify;
    verify = calloc(sizeof(int), 1);
    memcpy(verify, (char*) pageRecord + offset, sizeof(float));
    float offsetResult = *(float *) verify;
    free(verify);
    return offsetResult;
}

int IndexManager::getIntValueAtOffset(void *pageRecord, int offset) {
    void *verify;
    verify = calloc(sizeof(int), 1);
    memcpy(verify, (char*) pageRecord + offset, sizeof(int));
    int offsetResult = *(int *) verify;
    free(verify);
    return offsetResult;
}

RC IX_ScanIterator::getNextEntry(RID &rid, void *key)
{
	if(end) {
		// done, call close?
		return IX_EOF;
	}
	void* pageData = calloc(PAGE_SIZE, 1);

	//TODO: Take care of duplicate values for highkeyinclusive
	if(attribute.type == TypeInt) {
//		ixfileHandle->readPage(leafPageNum, scanPageData);
		int rawKey = getIntValueAtOffset(scanPageData, scanOffset);
		if(rawKey <= *(int*) highKey) {
			if(rawKey == *(int *)highKey && highKeyInclusive){
				end = true;
			} else if(rawKey == *(int *)highKey && !highKeyInclusive) {
				return IX_EOF;
			}
		} else {
			return IX_EOF;
		}
		memcpy(key, (char*) scanPageData + scanOffset, 4);
		memcpy(latestKey, (char*) key, 4);
		rid.pageNum = getIntValueAtOffset(scanPageData, scanOffset + 4);
		rid.slotNum = getIntValueAtOffset(scanPageData, scanOffset + 8);
		scanOffset += 12;
	} else if (attribute.type == TypeReal){
		cout << "In real" << endl;
		cout << "Scan offset: " << scanOffset << endl;
//		int result = ixfileHandle->readPage(leafPageNum, scanPageData);
//		if(result != 0)
//			cout << "Bad reading of page in scan::getNext"<<endl;
		float rawKey = getRealValueAtOffset(scanPageData, scanOffset);
		cout << "Raw key is: " << rawKey << endl;
		if(rawKey <= *(float *) highKey) {
			cout << "raw key is less" << endl;
			if(rawKey == *(float *)highKey && highKeyInclusive){
				end = true;
			} else if(rawKey == *(float *)highKey && !highKeyInclusive) {
				end = true;
				cout << "stopping 1" << endl;
				return IX_EOF;
			}
		} else {
			cout << "stopping 2" << endl;
			return IX_EOF;
		}
		cout << "Pass if" << endl;
		cout << "scanoffset" << scanOffset << endl;
		memcpy(key, (char*) scanPageData + scanOffset, 4);
		cout << "Key to be set: " << *(float *) key << endl;
//		memcpy(latestKey, (char*) key + 0, 4);
		rid.pageNum = getIntValueAtOffset(scanPageData, scanOffset + 4);
		rid.slotNum = getIntValueAtOffset(scanPageData, scanOffset + 8);
		scanOffset += 12;
		cout << "Scan offset: " << scanOffset << endl;

	}

	if(scanOffset >= indexManager->getEndOfRecordOffsetFromPage(scanPageData)) {
		// read the PAGE_SIZE - 6
		leafPageNum = getIntValueAtOffset(pageData, PAGE_SIZE - 8);
		ixfileHandle->readPage(leafPageNum, scanPageData);
		scanOffset = 0;
	}

    return 0;
}


int IX_ScanIterator::getIntValueAtOffset(void *pageRecord, int offset) {
    void *verify;
    verify = calloc(sizeof(int), 1);
    memcpy(verify, (char*) pageRecord + offset, sizeof(int));
    int offsetResult = *(int *) verify;
    free(verify);
    return offsetResult;
}

float IX_ScanIterator::getRealValueAtOffset(void *pageRecord, int offset) {
    void *verify;
    verify = calloc(sizeof(float), 1);
    memcpy(verify, (char*) pageRecord + offset, sizeof(float));
    float offsetResult = *(float *) verify;
    free(verify);
    return offsetResult;
}


RC IX_ScanIterator::close()
{
    return -1;
}


IXFileHandle::IXFileHandle()
{
    ixReadPageCounter = 0;
    ixWritePageCounter = 0;
    ixAppendPageCounter = 0;
//    FileHandle fileHandle;
}

IXFileHandle::~IXFileHandle()
{
}

RC IXFileHandle::collectCounterValues(unsigned &readPageCount, unsigned &writePageCount, unsigned &appendPageCount)
{
    readPageCount = this->readPageCounter;
    writePageCount = this->writePageCounter;
    appendPageCount = this->appendPageCounter;
    return 0;
}
