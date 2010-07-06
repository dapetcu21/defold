#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef __linux__
#include <limits.h>
#elif defined (__MACH__)
#include <sys/param.h>
#endif

#include <dlib/dstrings.h>
#include <dlib/hash.h>
#include <dlib/hashtable.h>
#include <dlib/log.h>

#include "resource.h"

/*
 * TODO:
 *
 *  - Resources could be loaded twice if canonical path is different for equivalent files.
 *    We could use realpath or similar function but we want to avoid file accesses when converting
 *    a canonical path to hash value. This functionality is used in the GetDescriptor function.
 *
 *  - If GetCanonicalPath exceeds RESOURCE_PATH_MAX PATH_TOO_LONG should be returned.
 *
 *  - Handle out of resources. Eg Hashtables full.
 */

namespace dmResource
{

struct SResourceType
{
    SResourceType()
    {
        memset(this, 0, sizeof(*this));
    }
    const char*       m_Extension;
    void*             m_Context;
    FResourceCreate   m_CreateFunction;
    FResourceDestroy  m_DestroyFunction;
    FResourceRecreate m_RecreateFunction;
};

// This is both used for m_ResourcePath in SResourceFactory and the total resource path, ie
// m_ResourcePath concatenated with relative path
const uint32_t RESOURCE_PATH_MAX = 1024;

const uint32_t MAX_RESOURCE_TYPES = 128;

struct SResourceFactory
{
    // TODO: Arg... budget. Two hash-maps. Really necessary?
    dmHashTable<uint64_t, SResourceDescriptor>*  m_Resources;
    dmHashTable<uintptr_t, uint64_t>*            m_ResourceToHash;
    // Only valid if RESOURCE_FACTORY_FLAGS_RELOAD_SUPPORT is set
    // Used for reloading of resources
    dmHashTable<uint64_t, const char*>*          m_ResourceHashToFilename;
    SResourceType                                m_ResourceTypes[MAX_RESOURCE_TYPES];
    uint32_t                                     m_ResourceTypesCount;
    char                                         m_ResourcePath[RESOURCE_PATH_MAX];
    char*                                        m_StreamBuffer;
    uint32_t                                     m_StreamBufferSize;
};

static SResourceType* FindResourceType(SResourceFactory* factory, const char* extension)
{
    for (uint32_t i = 0; i < factory->m_ResourceTypesCount; ++i)
    {
        SResourceType* rt = &factory->m_ResourceTypes[i];
        if (strcmp(extension, rt->m_Extension) == 0)
        {
            return rt;
        }
    }
    return 0;
}

// TODO: Test this...
static void GetCanonicalPath(const char* base_dir, const char* relative_dir, char* buf)
{
    DM_SNPRINTF(buf, RESOURCE_PATH_MAX, "%s/%s", base_dir, relative_dir);

    char* source = buf;
    char* dest = buf;
    char last_c = 0;
    while (*source != 0)
    {
        char c = *source;
        if (c != '/' || (c == '/' && last_c != '/'))
            *dest++ = c;

        last_c = c;
        ++source;
    }
    *dest = '\0';
}

void SetDefaultNewFactoryParams(struct NewFactoryParams* params)
{
    params->m_MaxResources = 1024;
    params->m_Flags = RESOURCE_FACTORY_FLAGS_EMPTY;
    params->m_StreamBufferSize = 4 * 1024 * 1024;
}

HFactory NewFactory(NewFactoryParams* params, const char* resource_path)
{
    void* buffer = malloc(params->m_StreamBufferSize);
    if (!buffer)
        return 0;

    SResourceFactory* factory = new SResourceFactory;
    factory->m_ResourceTypesCount = 0;
    factory->m_StreamBufferSize = params->m_StreamBufferSize;
    factory->m_StreamBuffer = (char*) buffer;

    const uint32_t table_size = (3 * params->m_MaxResources) / 4;
    factory->m_Resources = new dmHashTable<uint64_t, SResourceDescriptor>();
    factory->m_Resources->SetCapacity(table_size, params->m_MaxResources);

    factory->m_ResourceToHash = new dmHashTable<uintptr_t, uint64_t>();
    factory->m_ResourceToHash->SetCapacity(table_size, params->m_MaxResources);

    strncpy(factory->m_ResourcePath, resource_path, RESOURCE_PATH_MAX);
    factory->m_ResourcePath[RESOURCE_PATH_MAX-1] = '\0';

    if (params->m_Flags & RESOURCE_FACTORY_FLAGS_RELOAD_SUPPORT)
    {
        factory->m_ResourceHashToFilename = new dmHashTable<uint64_t, const char*>();
        factory->m_ResourceHashToFilename->SetCapacity(table_size, params->m_MaxResources);
    }
    else
    {
        factory->m_ResourceHashToFilename = 0;
    }

    return factory;
}

void DeleteFactory(HFactory factory)
{
    free(factory->m_StreamBuffer);
    delete factory->m_Resources;
    delete factory->m_ResourceToHash;
    delete factory->m_ResourceHashToFilename;
    delete factory;
}

FactoryResult RegisterType(HFactory factory,
                           const char* extension,
                           void* context,
                           FResourceCreate create_function,
                           FResourceDestroy destroy_function,
                           FResourceRecreate recreate_function)
{
    if (factory->m_ResourceTypesCount == MAX_RESOURCE_TYPES)
        return FACTORY_RESULT_OUT_OF_RESOURCES;

    // Dots not allowed in extension
    if (strrchr(extension, '.') != 0)
        return FACTORY_RESULT_INVAL;

    if (create_function == 0 || destroy_function == 0)
        return FACTORY_RESULT_INVAL;

    if (FindResourceType(factory, extension) != 0)
        return FACTORY_RESULT_ALREADY_REGISTERED;

    SResourceType resource_type;
    resource_type.m_Extension = extension;
    resource_type.m_Context = context;
    resource_type.m_CreateFunction = create_function;
    resource_type.m_DestroyFunction = destroy_function;
    resource_type.m_RecreateFunction = recreate_function;

    factory->m_ResourceTypes[factory->m_ResourceTypesCount++] = resource_type;

    return FACTORY_RESULT_OK;
}

static FactoryResult LoadResource(HFactory factory, const char* path, uint32_t* resource_size)
{
    FILE* f = fopen(path, "rb");
    if (f == 0)
    {
        dmLogWarning("Resource not found: %s", path);
        return FACTORY_RESULT_RESOURCE_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    *resource_size = (uint32_t) file_size;

    // Extra byte for resources expecting null-terminated string...
    if (file_size + 1 >= (long) factory->m_StreamBufferSize)
    {
        dmLogError("Resource too large for streambuffer: %s", path);
        fclose(f);
        return FACTORY_RESULT_STREAMBUFFER_TOO_SMALL;
    }
    factory->m_StreamBuffer[file_size] = 0; // Null-terminate. See comment above

    if (fread(factory->m_StreamBuffer, 1, file_size, f) != (size_t) file_size)
    {
        fclose(f);
        return FACTORY_RESULT_IO_ERROR;
    }

    fclose(f);
    return FACTORY_RESULT_OK;
}

FactoryResult Get(HFactory factory, const char* name, void** resource)
{
    assert(name);
    assert(resource);

    *resource = 0;

#if 1
    char canonical_path[RESOURCE_PATH_MAX];
    GetCanonicalPath(factory->m_ResourcePath, name, canonical_path);

#else
#ifdef _WIN32
    char canonical_path[_PATH_MAX];
    char *canonical_path_p = _fullpath(canonical_path, name, _PATH_MAX);
#else
    char canonical_path[PATH_MAX];
    char *canonical_path_p = realpath(name, canonical_path);
#endif

    if (canonical_path_p == 0)
    {
        return FACTORY_RESULT_RESOURCE_NOT_FOUND;
    }
#endif

    uint64_t canonical_path_hash = dmHashBuffer64(canonical_path, strlen(canonical_path));

    SResourceDescriptor* rd = factory->m_Resources->Get(canonical_path_hash);
    if (rd)
    {
        assert(factory->m_ResourceToHash->Get((uintptr_t) rd->m_Resource));
        rd->m_ReferenceCount++;
        *resource = rd->m_Resource;
        return FACTORY_RESULT_OK;
    }

    const char* ext = strrchr(name, '.');
    if (ext)
    {
        ext++;

        SResourceType* resource_type = FindResourceType(factory, ext);
        if (resource_type == 0)
        {
            dmLogError("Unknown resource type: %s", ext);
            return FACTORY_RESULT_UNKNOWN_RESOURCE_TYPE;
        }

        uint32_t file_size;
        FactoryResult result = LoadResource(factory, canonical_path, &file_size);
        if (result != FACTORY_RESULT_OK)
            return result;

        struct stat file_stat;
        stat(canonical_path, &file_stat);
        // TODO: Fix better resolution on this.
        uint32_t mtime = (uint32_t) file_stat.st_mtime;

        // TODO: We should *NOT* allocate SResource dynamically...
        SResourceDescriptor tmp_resource;
        memset(&tmp_resource, 0, sizeof(tmp_resource));
        tmp_resource.m_NameHash = canonical_path_hash;
        tmp_resource.m_ReferenceCount = 1;
        tmp_resource.m_ResourceType = (void*) resource_type;
        tmp_resource.m_ModificationTime = mtime;

        CreateResult create_error = resource_type->m_CreateFunction(factory, resource_type->m_Context, factory->m_StreamBuffer, file_size, &tmp_resource, name);

        if (create_error == CREATE_RESULT_OK)
        {
            assert(tmp_resource.m_Resource); // TODO: Or handle gracefully!
            factory->m_Resources->Put(canonical_path_hash, tmp_resource);
            factory->m_ResourceToHash->Put((uintptr_t) tmp_resource.m_Resource, canonical_path_hash);
            if (factory->m_ResourceHashToFilename)
            {
                factory->m_ResourceHashToFilename->Put(canonical_path_hash, strdup(canonical_path));
            }
            *resource = tmp_resource.m_Resource;
            return FACTORY_RESULT_OK;
        }
        else
        {
            dmLogWarning("Unable to create resource: %s", canonical_path);
            // TODO: Map CreateResult to FactoryResult here.
            return FACTORY_RESULT_UNKNOWN;
        }
    }
    else
    {
        return FACTORY_RESULT_MISSING_FILE_EXTENSION;
    }
}

struct ReloadTypeContext
{
    HFactory      m_Factory;
    uint32_t      m_Type;
    FactoryResult m_Result;
};

void ReloadTypeCallback(ReloadTypeContext* context, const uint64_t* resource_hash, const char** file_name)
{
    if (context->m_Result != FACTORY_RESULT_OK)
        return;

    SResourceDescriptor* rd = context->m_Factory->m_Resources->Get(*resource_hash);
    assert(rd);

    SResourceType* resource_type = (SResourceType*) rd->m_ResourceType;
    // TODO: Not 64-bit friendly
    if ((uint32_t) resource_type == context->m_Type)
    {

        uint32_t file_size;
        FactoryResult result = LoadResource(context->m_Factory, *file_name, &file_size);
        if (result != FACTORY_RESULT_OK)
        {
            context->m_Result = result;
            return;
        }

        struct stat file_stat;
        if (stat(*file_name, &file_stat) != 0)
        {
            context->m_Result = FACTORY_RESULT_RESOURCE_NOT_FOUND;
            return;
        }
        // TODO: Fix better resolution on this.
        uint32_t mtime = (uint32_t) file_stat.st_mtime;

        if (mtime == rd->m_ModificationTime)
            return;

        // TODO: We should *NOT* allocate SResource dynamically...
        CreateResult create_result = resource_type->m_RecreateFunction(context->m_Factory, resource_type->m_Context, context->m_Factory->m_StreamBuffer, file_size, rd, *file_name);

        if (create_result != CREATE_RESULT_OK)
        {
            context->m_Result = FACTORY_RESULT_UNKNOWN;
        }
    }
}

FactoryResult ReloadType(HFactory factory, uint32_t type)
{
    ReloadTypeContext context;
    context.m_Factory = factory;
    context.m_Type = type;
    context.m_Result = FACTORY_RESULT_OK;
    factory->m_ResourceHashToFilename->Iterate(&ReloadTypeCallback, &context);

    return context.m_Result;
}

FactoryResult GetType(HFactory factory, void* resource, uint32_t* type)
{
    assert(type);

    uint64_t* resource_hash = factory->m_ResourceToHash->Get((uintptr_t) resource);
    if (!resource_hash)
    {
        return FACTORY_RESULT_NOT_LOADED;
    }

    SResourceDescriptor* rd = factory->m_Resources->Get(*resource_hash);
    assert(rd);
    assert(rd->m_ReferenceCount > 0);
    *type = (uint32_t) rd->m_ResourceType; // TODO: Not 64-bit friendly...

    return FACTORY_RESULT_OK;
}

FactoryResult GetTypeFromExtension(HFactory factory, const char* extension, uint32_t* type)
{
    assert(type);

    SResourceType* resource_type = FindResourceType(factory, extension);
    if (resource_type)
    {
        *type = (uint32_t) resource_type; // TODO: Not 64-bit friendly...
        return FACTORY_RESULT_OK;
    }
    else
    {
        return FACTORY_RESULT_UNKNOWN_RESOURCE_TYPE;
    }
}

FactoryResult GetExtensionFromType(HFactory factory, uint32_t type, const char** extension)
{
    for (uint32_t i = 0; i < factory->m_ResourceTypesCount; ++i)
    {
        SResourceType* rt = &factory->m_ResourceTypes[i];

        if (((uintptr_t) rt) == type)
        {
            *extension = rt->m_Extension;
            return FACTORY_RESULT_OK;
        }
    }

    *extension = 0;
    return FACTORY_RESULT_UNKNOWN_RESOURCE_TYPE;
}

FactoryResult GetDescriptor(HFactory factory, const char* name, SResourceDescriptor* descriptor)
{
    char canonical_path[RESOURCE_PATH_MAX];
    GetCanonicalPath(factory->m_ResourcePath, name, canonical_path);

    uint64_t canonical_path_hash = dmHashBuffer64(canonical_path, strlen(canonical_path));

    SResourceDescriptor* tmp_descriptor = factory->m_Resources->Get(canonical_path_hash);
    if (tmp_descriptor)
    {
        *descriptor = *tmp_descriptor;
        return FACTORY_RESULT_OK;
    }
    else
    {
        return FACTORY_RESULT_NOT_LOADED;
    }
}

void Acquire(HFactory factory, void* resource)
{
    uint64_t* resource_hash = factory->m_ResourceToHash->Get((uintptr_t) resource);
    assert(resource_hash);

    SResourceDescriptor* rd = factory->m_Resources->Get(*resource_hash);
    assert(rd);
    assert(rd->m_ReferenceCount > 0);
    rd->m_ReferenceCount++;
}

void Release(HFactory factory, void* resource)
{
    uint64_t* resource_hash = factory->m_ResourceToHash->Get((uintptr_t) resource);
    assert(resource_hash);

    SResourceDescriptor* rd = factory->m_Resources->Get(*resource_hash);
    assert(rd);
    assert(rd->m_ReferenceCount > 0);
    rd->m_ReferenceCount--;

    if (rd->m_ReferenceCount == 0)
    {
        SResourceType* resource_type = (SResourceType*) rd->m_ResourceType;
        resource_type->m_DestroyFunction(factory, resource_type->m_Context, rd);

        factory->m_ResourceToHash->Erase((uintptr_t) resource);
        factory->m_Resources->Erase(*resource_hash);
        if (factory->m_ResourceHashToFilename)
        {
            const char** s = factory->m_ResourceHashToFilename->Get(*resource_hash);
            assert(s);
            free((void*) *s);
        }
    }
}


}

