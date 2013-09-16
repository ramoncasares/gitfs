
#include <assert.h>

#define FUSE_USE_VERSION 27
#include <fuse.h>

#include <cache.h>
#include <object.h>
#include <tree.h>
#include <commit.h>
#include <tag.h>
#include <diff.h>
#include <revision.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <attr/xattr.h>


struct __gitfs_object {
    unsigned char rev[20];
    struct object *obj;
    unsigned mode;
    unsigned long size;

    unsigned char commit[20];
    time_t date;
    unsigned no_parents;

    void *data;
};

const char *prefix;
static time_t mountdate;
static uid_t uid; static gid_t gid;

static const char *parse(const char *path, unsigned char sha1[20])
{
    const char *ref, *end; ref = end = path;
    char name[strlen(path) + 1];

    if ( path[0] != '/'  ) return NULL;
    if ( path[1] == '\0' ) return NULL;

    ref = path + 1;
    end = strchr(ref, '/');
    if (end == NULL) end = ref + strlen(ref);
    else {
        ref = end;
        do {
            if (ref[0] == '\0') { end = ref; break; }
            if (ref[0] == '/' ) end = ref; else break;
            ref++;
            if ( ref[0] != '^' && ref[0] != '~' ) break;
            ref++;
            do {
                if ( ref[0] != '^' && ref[0] != '~' && ref[0] != '0' &&
                     ref[0] != '1' && ref[0] != '2' && ref[0] != '3' &&
                     ref[0] != '4' && ref[0] != '5' && ref[0] != '6' &&
                     ref[0] != '7' && ref[0] != '8' && ref[0] != '9' ) break;
                ref++;
            } while (1);
        } while (1);
    }

    int j = 0;
    for(; path<end; path++)
        if(path[0] != '/')
           if(path[0] == ':') name[j++] = '/'; else name[j++] = path[0];
    name[j] = '\0';

    path = end[0] == '/' ? end + 1 : end;

    if (get_sha1(name, sha1)) return NULL;

    return path;
}

struct object *gitobj(unsigned char sha1[20], unsigned long *date, unsigned *no_parents, unsigned char commit[20])
{
    struct object *obj = parse_object(sha1);
        do {
                if (!obj)
                        return NULL;
                if (obj->type == OBJ_TREE || obj->type == OBJ_BLOB)
                        return obj;
                else if (obj->type == OBJ_COMMIT) {
                        *date = ((struct commit *) obj)->date;
                        *no_parents = commit_list_count(((struct commit *) obj)->parents);
                        int i; for(i=0; i<20; i++) commit[i] = obj->sha1[i];
                        obj = &(((struct commit *) obj)->tree->object);
                }
                else if (obj->type == OBJ_TAG) {
                        *date = ((struct tag *) obj)->date;
                        obj = ((struct tag *) obj)->tagged;
                }
                else
                        return NULL;

                if (!obj->parsed)
                        parse_object(obj->sha1);
        } while (1);
}

struct __gitfs_object *__gitfs(const char *path)
{
    unsigned char sha1[20];
    const char *dir = parse(path, sha1);
    if (!dir)
        return NULL;

    unsigned long date = mountdate;
    unsigned char commit[20];
    unsigned no_parents = 0;
    struct object *obj = gitobj(sha1, &date, &no_parents, commit);
    if (!obj)
        return NULL;

    unsigned mode = 0644;
    if (obj->type == OBJ_TREE) {
        unsigned char blob[20];
        if (get_tree_entry(sha1, dir, blob, &mode))
            return NULL;

        obj = parse_object(blob);
    }

    if (dir[0] != '\0') no_parents = 0;

    struct __gitfs_object *gfsobj = malloc(sizeof(struct __gitfs_object));
    if (gfsobj) {
        hashcpy(gfsobj->rev, sha1);
        gfsobj->obj = obj;
        gfsobj->mode = mode & ~0222;
        gfsobj->date = date;
        int i; for(i=0; i<20; i++) gfsobj->commit[i] = commit[i];
        gfsobj->no_parents = no_parents;
    }
    return gfsobj;
}

static int __gitfs_getattr(const char *path, struct stat *stbuf)
{
    int res = 0;

    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0555;
        stbuf->st_nlink = 2;
        stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = mountdate;
        stbuf->st_uid = uid; stbuf->st_gid = gid;
        return 0;
    }

    struct __gitfs_object *obj = __gitfs(path);
    if (obj == NULL)
        return -ENOENT;

    if (obj->obj->type == OBJ_TREE) {
        stbuf->st_mode = S_IFDIR | 0555;
        stbuf->st_nlink = 2;
        stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = obj->date;
        stbuf->st_uid = uid; stbuf->st_gid = gid;

    } else if (obj->obj->type == OBJ_BLOB) {
        stbuf->st_mode = obj->mode & ~0222;
        stbuf->st_nlink = 1;

        stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = obj->date;
        stbuf->st_uid = uid; stbuf->st_gid = gid;
        unsigned long size;
        sha1_object_info(obj->obj->sha1, &size);

        stbuf->st_size = size;
    } else {
        res = -ENOENT;
    }

    free(obj);

    return res;
}

static int __gitfs_readlink(const char *path, char *buf, size_t size)
{
    unsigned long len;

    struct __gitfs_object *obj = __gitfs(path);
    if (!obj)
        return -ENOENT;
    
    enum object_type type;
    void *data = read_sha1_file(obj->obj->sha1, &type, &len);

    assert(type == obj->obj->type);
    free(obj);

    if (len > size)
        len = size;

    memcpy(buf, data, len);
    free(data);

    return 0;
}

static int __gitfs_open(const char *path, struct fuse_file_info *fi)
{
    struct __gitfs_object *obj = __gitfs(path);
    if (!obj)
        return -ENOENT;

    if (obj->obj->type == OBJ_TREE)
        return -EISDIR;

    assert(obj->obj->type == OBJ_BLOB);

    if ((fi->flags & 3) != O_RDONLY)
        return -EACCES;

    enum object_type type;
    obj->data = read_sha1_file(obj->obj->sha1, &type, &obj->size);

    assert(type == obj->obj->type);

    fi->fh = (uint64_t) obj;

    return 0;
}

static int __gitfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct __gitfs_object *obj = (struct __gitfs_object *) fi->fh;
    assert(obj);

    if (obj->obj->type != OBJ_BLOB)
        return -ENOENT;
    
    if (offset < obj->size) {
        if (offset + size > obj->size)
            size = obj->size - offset;
        memcpy(buf, obj->data + offset, size);
    } else {
        size = 0;
    }

    return size;
}

static int __gitfs_release(const char *path, struct fuse_file_info *fi)
{
    struct __gitfs_object *obj = (struct __gitfs_object *) fi->fh;
    assert(obj);

    free(obj->data);
    free(obj);

    return 0;
}


static int __gitfs_listxattr(const char *path, char *list, size_t size)
{
    static char *attr[] = { "user.sha1" , "user.commit" };
    int no_attrs = sizeof(attr) / sizeof(*attr);
    int len = 0;
    int i; for(i=0; i<no_attrs; i++) len = len + strlen(attr[i]) + 1;

    if (size == 0) return len;

    if (strcmp(path, "/") == 0) {
        list[0] = '\0';
        return 1;
    } else {
        if (size < len) return -ERANGE;
        char *s = list; char *p;
        for(i=0; i<no_attrs; i++) {
            p = attr[i];
            while(*s++ = *p++) ;
        }
        return len;
    }
}

static int __gitfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
    unsigned long len = 41;
    if (size == 0) return len;

    if (strcmp(path, "/") == 0) return 0;

    struct __gitfs_object *obj = __gitfs(path);
    if (!obj) return -ENOENT;

    if (strcmp(name, "user.sha1") == 0) {
        if (len > size) return -ERANGE;
        strncpy(value, sha1_to_hex(obj->obj->sha1), len);
        return len;
    } else if (strcmp(name, "user.commit") == 0) {
        if (len > size) return -ERANGE;
        strncpy(value, sha1_to_hex(obj->commit), len);
        return len;
    } else return -ENOATTR;
}

static int __gitfs_opendir(const char *path, struct fuse_file_info *fi)
{
    return -ENOENT;
}


struct __gitfs_readdir_ctx {
    void *buf;
    fuse_fill_dir_t filler;
};

static int show_tree(const unsigned char *sha1, const char *base, int baselen,
             const char *pathname, unsigned int mode, int stage, void *context)
{
    struct __gitfs_readdir_ctx *ctx = context;

    (*ctx->filler) (ctx->buf, pathname, NULL, 0);

    return 0;
}

static int show_ref(const char *refname, const unsigned char *sha1, int flag, void *context)
{
    struct __gitfs_readdir_ctx *ctx = context;

    char name[strlen(refname) + 1]; int i = 0;
    const char *p; p = refname;
    while (*p) {
        if ( *p == '/' ) name[i++] = ':'; else name[i++] = *p;
        p++;
    }
    name[i] = '\0';

    (*ctx->filler) (ctx->buf, name, NULL, 0);

    return 0;
}

static int show_commit(struct commit *cm, void *context)
{
    struct __gitfs_readdir_ctx *ctx = context;

    (*ctx->filler) (ctx->buf, sha1_to_hex(cm->object.sha1), NULL, 0);

    return 0;
}

static void rev_list_all(struct rev_info *revs)
{
    init_revisions(revs, prefix);
    const char *args[] = { "--all", "HEAD" };
    setup_revisions(2, args, revs, NULL);
    reset_revision_walk();
    prepare_revision_walk(revs);
}

static int __gitfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct __gitfs_readdir_ctx ctx = { buf, filler };

    if (strcmp(path, "/") == 0) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);

        filler(buf, "HEAD", NULL, 0);
        // for_each_ref(show_ref, &ctx);
        for_each_tag_ref(show_ref, &ctx);
        for_each_branch_ref(show_ref, &ctx);
        for_each_remote_ref(show_ref, &ctx);
        for_each_replace_ref(show_ref, &ctx);

        struct rev_info revs;
        rev_list_all(&revs);
        traverse_commit_list(&revs, show_commit, NULL, &ctx);

        return 0;
    }

    struct __gitfs_object *obj = __gitfs(path);
    if (!obj || obj->obj->type != OBJ_TREE)
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    unsigned no_parents = obj->no_parents;
    char np[11];
    if (no_parents == 1) filler(buf, "^", NULL, 0);
    else while (no_parents > 0) {
      sprintf(np, "^%u", no_parents);
      filler(buf, np, NULL, 0);
      no_parents--;
    }

    struct tree *tree = (struct tree *) obj->obj;

    struct pathspec pathspec;
    init_pathspec(&pathspec, NULL);

    read_tree_recursive(tree, "", 0, 0, &pathspec, show_tree, &ctx);

    free(obj);

    return 0;
}

static int __gitfs_releasedir(const char *path, struct fuse_file_info *fi)
{
    return -ENOENT;
}

static int __gitfs_access(const char *path, int mode)
{
    return -ENOENT;
}

static int __gitfs_fgetattr(const char *path, struct stat *buf, struct fuse_file_info *fi)
{
    return -ENOENT;
}

static struct fuse_operations __gitfs_ops = {
    .getattr    = __gitfs_getattr,
    .readlink   = __gitfs_readlink,
    .open       = __gitfs_open,
    .read       = __gitfs_read,
    .release    = __gitfs_release,
    .getxattr   = __gitfs_getxattr,
    .listxattr  = __gitfs_listxattr,
//    .opendir    = __gitfs_opendir,
    .readdir    = __gitfs_readdir,
//    .releasedir = __gitfs_releasedir,
//    .access     = __gitfs_access,
//    .fgetattr   = __gitfs_fgetattr,
};

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    prefix = setup_git_directory();
    git_config(git_default_config, NULL);
    mountdate = time(NULL);
    uid = getuid(); gid = getgid();

    return fuse_main(args.argc, args.argv, &__gitfs_ops, NULL);
}
