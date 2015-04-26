#include "loop.h"
#include "debugger.h"
#include "memory.h"
#include "threading.h"
#include "module.h"

std::map<DepthModuleRange, LOOPSINFO, DepthModuleRangeCompare> loops;

bool LoopAdd(uint Start, uint End, bool Manual)
{
    // CHECK: Export function
    if(!DbgIsDebugging())
        return false;

    // Loop must begin before it ends
    if(Start > End)
        return false;

    // Memory addresses must be valid
    if(!MemIsValidReadPtr(Start) || !MemIsValidReadPtr(End))
        return false;

    // Check if loop boundaries are in the same module range
    const uint moduleBase = ModBaseFromAddr(Start);

    if(moduleBase != ModBaseFromAddr(End))
        return false;

    // Loops cannot overlap other loops
    int finalDepth = 0;

    if(LoopOverlaps(0, Start, End, &finalDepth))
        return false;

    // Fill out loop information structure
    LOOPSINFO loopInfo;
    loopInfo.start = Start - moduleBase;
    loopInfo.end = End - moduleBase;
    loopInfo.depth = finalDepth;
    loopInfo.manual = Manual;
    ModNameFromAddr(Start, loopInfo.mod, true);

    // Link this to a parent loop if one does exist
    if(finalDepth)
        LoopGet(finalDepth - 1, Start, &loopInfo.parent, 0);
    else
        loopInfo.parent = 0;

    EXCLUSIVE_ACQUIRE(LockLoops);

    // Insert into list
    loops.insert(std::make_pair(DepthModuleRange(finalDepth, ModuleRange(ModHashFromAddr(moduleBase), Range(loopInfo.start, loopInfo.end))), loopInfo));
    return true;
}

// Get the start/end of a loop at a certain depth and address
bool LoopGet(int Depth, uint Address, uint* Start, uint* End)
{
    // CHECK: Exported function
    if(!DbgIsDebugging())
        return false;

    // Get the virtual address module
    const uint moduleBase = ModBaseFromAddr(Address);

    // Virtual address to relative address
    Address -= moduleBase;

    SHARED_ACQUIRE(LockLoops);

    // Search with this address range
    auto found = loops.find(DepthModuleRange(Depth, ModuleRange(ModHashFromAddr(moduleBase), Range(Address, Address))));

    if(found == loops.end())
        return false;

    // Return the loop start
    if(Start)
        *Start = found->second.start + moduleBase;

    // Also the loop end
    if(End)
        *End = found->second.end + moduleBase;

    return true;
}

//check if a loop overlaps a range, inside is not overlapping
bool LoopOverlaps(int Depth, uint Start, uint End, int* FinalDepth)
{
    // CHECK: Export function
    if(!DbgIsDebugging())
        return false;

    // Determine module addresses and lookup keys
    const uint moduleBase = ModBaseFromAddr(Start);
    const uint key = ModHashFromAddr(moduleBase);

    uint curStart = Start - moduleBase;
    uint curEnd = End - moduleBase;

    SHARED_ACQUIRE(LockLoops);

    // Check if the new loop fits in the old loop
    for(auto & itr : loops)
    {
        // Only look in the current module
        if(itr.first.second.first != key)
            continue;

        // Loop must be at this recursive depth
        if(itr.second.depth != Depth)
            continue;

        if(itr.second.start < curStart && itr.second.end > curEnd)
            return LoopOverlaps(Depth + 1, curStart, curEnd, FinalDepth);
    }

    // Did the user request t the loop depth?
    if(FinalDepth)
        *FinalDepth = Depth;

    // Check for loop overlaps
    for(auto & itr : loops)
    {
        // Only look in the current module
        if(itr.first.second.first != key)
            continue;

        // Loop must be at this recursive depth
        if(itr.second.depth != Depth)
            continue;

        if(itr.second.start <= curEnd && itr.second.end >= curStart)
            return true;
    }

    return false;
}

// This should delete a loop and all sub-loops that matches a certain addr
bool LoopDelete(int Depth, uint Address)
{
    return false;
}

void LoopCacheSave(JSON Root)
{
    EXCLUSIVE_ACQUIRE(LockLoops);

    // Create the root JSON objects
    const JSON jsonLoops = json_array();
    const JSON jsonAutoLoops = json_array();

    // Write all entries
    for(auto & itr : loops)
    {
        const LOOPSINFO & currentLoop = itr.second;
        JSON currentJson = json_object();

        json_object_set_new(currentJson, "module", json_string(currentLoop.mod));
        json_object_set_new(currentJson, "start", json_hex(currentLoop.start));
        json_object_set_new(currentJson, "end", json_hex(currentLoop.end));
        json_object_set_new(currentJson, "depth", json_integer(currentLoop.depth));
        json_object_set_new(currentJson, "parent", json_hex(currentLoop.parent));

        if(currentLoop.manual)
            json_array_append_new(jsonLoops, currentJson);
        else
            json_array_append_new(jsonAutoLoops, currentJson);
    }

    // Append a link to the global root
    if(json_array_size(jsonLoops))
        json_object_set(Root, "loops", jsonLoops);

    if(json_array_size(jsonAutoLoops))
        json_object_set(Root, "autoloops", jsonAutoLoops);

    // Release memory/references
    json_decref(jsonLoops);
    json_decref(jsonAutoLoops);
}

void LoopCacheLoad(JSON Root)
{
    EXCLUSIVE_ACQUIRE(LockLoops);

    // Inline lambda to parse each JSON entry
    auto AddLoops = [](const JSON Object, bool Manual)
    {
        size_t i;
        JSON value;

        json_array_foreach(Object, i, value)
        {
            LOOPSINFO loopInfo;
            memset(&loopInfo, 0, sizeof(LOOPSINFO));

            // Module name
            const char* mod = json_string_value(json_object_get(value, "module"));

            if(mod && strlen(mod) < MAX_MODULE_SIZE)
                strcpy_s(loopInfo.mod, mod);
            else
                loopInfo.mod[0] = '\0';

            // All other variables
            loopInfo.start = (uint)json_hex_value(json_object_get(value, "start"));
            loopInfo.end = (uint)json_hex_value(json_object_get(value, "end"));
            loopInfo.depth = (int)json_integer_value(json_object_get(value, "depth"));
            loopInfo.parent = (uint)json_hex_value(json_object_get(value, "parent"));
            loopInfo.manual = Manual;

            // Sanity check: Make sure the loop starts before it ends
            if(loopInfo.end < loopInfo.start)
                continue;

            // Insert into global list
            loops.insert(std::make_pair(DepthModuleRange(loopInfo.depth, ModuleRange(ModHashFromName(loopInfo.mod), Range(loopInfo.start, loopInfo.end))), loopInfo));
        }
    };

    // Remove existing entries
    loops.clear();

    const JSON jsonLoops = json_object_get(Root, "loops");
    const JSON jsonAutoLoops = json_object_get(Root, "autoloops");

    // Load user-set loops
    if(jsonLoops)
        AddLoops(jsonLoops, true);

    // Load auto-set loops
    if(jsonAutoLoops)
        AddLoops(jsonAutoLoops, false);
}

bool LoopEnum(LOOPSINFO* List, size_t* Size)
{
    // If list or size is not requested, fail
    if(!List && !Size)
        return false;

    SHARED_ACQUIRE(LockLoops);

    // See if the caller requested an output size
    if(Size)
    {
        *Size = loops.size() * sizeof(LOOPSINFO);

        if(!List)
            return true;
    }

    for(auto & itr : loops)
    {
        *List = itr.second;

        // Adjust the offset to a real virtual address
        uint modbase = ModBaseFromName(List->mod);
        List->start += modbase;
        List->end += modbase;

        List++;
    }

    return true;
}

void LoopClear()
{
    EXCLUSIVE_ACQUIRE(LockLoops);
    loops.clear();
}