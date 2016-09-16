#include "mrepro.h"

#define PORT_DEFAULT "80"
#define ROOT_DEFAULT "." // current directory
#define PATH_LEN     256
#define MAX_THREAD   256
#define WAIT_SECS    300 // wait 300 seconds
#define DEFAULT_TYPE "application/octet-stream"

void Usage(const char* name){
    Errx(MP_PARAM_ERR, "%s [-d] [-r root_dir] [tcp_port [udp_port]]", name);
}

char* Status(int);
void WriteHeader(int, int, int, int, const char*);
void HttpError(int, int);

void CheckRootDir(const char* dir){
	if (!strncmp(dir, "/", 2)    || !strncmp(dir, "/etc", 5) ||
		!strncmp(dir, "/bin", 5) || !strncmp(dir, "/lib", 5) ||
		!strncmp(dir, "/tmp", 5) || !strncmp(dir, "/usr", 5) ||
		!strncmp(dir, "/dev", 5) || !strncmp(dir, "/sbin", 6))
		Errx(MP_PARAM_ERR, "root dir can't be %s", dir);
}

struct {
    char* ext;
    char* type;
} extensions [] = {

    { "bin",    "application/octet-stream"  },
    { "pdf",    "application/pdf"           },
    { "dvi",    "application/x-dvi"         },
    { "tex",    "application/x-tex"         },
    { "mp3",    "audio/x-mpeg"              },
    { "sh",     "application/x-sh"          },

    // images
    { "gif",    "image/gif"                 },
    { "jpg",    "image/jpeg"                },
    { "jpeg",   "image/jpeg"                },
    { "png",    "image/png"                 },

    // archives
    { "zip",    "application/zip"           },
    { "gz",     "application/x-gzip"        },
    { "gzip",   "application/x-gzip"        },
    { "tar",    "application/x-tar"         },
    { "jar",    "application/java-archive"  },

    // text
    { "htm",    "text/html"                 },
    { "html",   "text/html"                 },
    { "c",      "text/plain"                },
    { "cc",     "text/plain"                },
    { "h",      "text/plain"                },
    { "txt",    "text/plain"                },
    { 0,        0                           }
};

// FILE

char* GetType(const char* path){
	int i, len = strlen(path);
    const char* ext;

    if (!strcmp(path, "Makefile") || !strcmp(path, "makefile"))
        return "text/plain";

    for ( i = len-1; i >=0; --i ) {
        if (path[i] == '.') break;
    }
    if (i==-1 || i==len-1) return NULL; // last or first = '.'

    ext  = path + i + 1;

    for ( i = 0; extensions[i].ext; i++ ){
        if ( !strcmp(ext, extensions[i].ext) )
            return extensions[i].type;
    }

    return NULL;
}

void GetFile(int socket, const char* filename){
    // filename  ./dir/file.txt
    struct stat st;
    char* type = GetType(filename);
    if (type == NULL)
        type = DEFAULT_TYPE;

    stat(filename, &st);
    WriteHeader(socket, 200, 0, st.st_size, type);
    TransferFile(socket, filename, 0);
}

// DIRECTORY

char* FileLink(const char* path, const char* file, off_t size){
    char* buff = MLC(char, strlen(path) + strlen(file) + 30);
    long lsize = (long) size;
    sprintf(buff, "<a href=\"%s\">%s (%ld)</a><br>", path, file, lsize);
    return buff;
}

char* DirLink(const char* dir, const char* preview){
    char* buff = MLC(char, strlen(dir) + strlen(preview) + 30);
    sprintf(buff, "<a href=\"%s\">%s [dir]</a><br>", dir, preview);
    return buff;
}

void GetDir(int socket, const char* dirname){

    // open tmp file
    int tmp_file;
    char* tmp_filename = MLC(char, 20);
	strcpy(tmp_filename, "/tmp/httpXXXXXX");
	if ( (tmp_file = mkstemp(tmp_filename)) == -1 ){
        HttpError(socket, 500);
        free(tmp_filename);
        return;
    }

    DIR *dir;
    struct dirent *ent;
    struct stat st;
    char* ptr;

    char* path = MLC(char, BUFFER_LEN_SMALL);
    char* nameptr;
    int i, len = strlen(dirname);
    char tmp_char;

    ptr = "<html><title>MrePro web server</title><body><h3>Listing for ";
    Writen(tmp_file, ptr, strlen(ptr));
    Writen(tmp_file, dirname+1, len-1);
    ptr = "</h3><p>";
    Writen(tmp_file, ptr, strlen(ptr));

    strcpy(path, dirname);
    if (dirname[len-1] != '/')
        path[len++] = '/';
    nameptr = path + len;

    // dirname  ./dir
    // path     ./dir/
    // nameptr -------A (pointer for file name insertion)

    dir = opendir(dirname);

    while ( (ent=readdir(dir)) != NULL ) {
        // skip .
        if (!strcmp(".", ent->d_name))
            continue;

        if (!strcmp("..", ent->d_name)){
            // only ignore .. in root
            if (!strcmp("./", dirname))
                continue;

            // find first right '/'
            for(i=strlen(dirname)-1; dirname[i]!='/'; --i);
            if (i==1)
                ptr = DirLink("/", "..");
            else {
                tmp_char = path[i];
                path[i] = 0;
                ptr = DirLink(path+1, "..");
                path[i] = tmp_char;
            }

        } else {
            strcpy(nameptr, ent->d_name);
            if (stat(path, &st)) continue;

            if (S_ISDIR(st.st_mode))
                ptr = DirLink(path+1, ent->d_name);
            else
                ptr = FileLink(path+1, ent->d_name, st.st_size);
        }

        // write ptr to file
        Writen(tmp_file, ptr, strlen(ptr));
        free(ptr);
    }
    closedir (dir);

    ptr = "</p></body></html>";
    Writen(tmp_file, ptr, strlen(ptr));
    close(tmp_file);

    stat(tmp_filename, &st);

    WriteHeader(socket, 200, 0, st.st_size, "text/html");
    TransferFile(socket, tmp_filename, 0);

    unlink(tmp_filename);
    free(path);
    free(tmp_filename);
}

// GET

void RemoveIndex(char* path){
    int len = strlen(path);
    const char* IDX = "index.html";
    const int idx_len = strlen(IDX);

    if (!strncmp( path+len-idx_len+1, IDX, idx_len ))
        path[len-idx_len+1] = 0;
}

void Get(int socket, char* path){
    int i, len = strlen(path);
    char* dot = MLC(char, len+2);

    RemoveIndex(path);

    if (path[0] != '/')
        sprintf(dot, "./%s", path);
    else
        sprintf(dot, ".%s", path);

    for(i=0; i<len-1; i++){
        if (path[i]=='.' && path[i+1]=='.'){
            HttpError(socket, 400);
            free(dot);
            return;
        }
    }

    struct stat st;
    if (access(dot, F_OK)){
        HttpError(socket, 404);
        free(dot);
        return;
    }
    stat(dot,&st);

    if (S_ISDIR(st.st_mode))
        GetDir(socket, dot);
    else
        GetFile(socket, dot);

    free(dot);
}
