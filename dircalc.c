#include <stdio.h>
#include <windows.h>
#include <stdlib.h>

unsigned long long size, filesize, dirsize, filenumber, filedir, dirnumber, minsize;
char s[] = "kMGTPE";
char temp[3000];
char *dir;
int i, access, level, limit, dironly;
float n;

const char* format(unsigned long long number)
{
    int i, j;
    static char text1[30], text2[30];
    
    sprintf(text1, "%llu", number);
    sprintf(text2, "");

     j = 0;
    for (i = 0; text1[i] != '\0'; i++)
    {
        if (((strlen(text1) - i) % 3 == 0) && (j != 0))
            {
                text2[j] = ' '; 
                j++;
            }
        text2[j] = text1[i];
        j++;
    }
    text2[j] = '\0';
    if (access == 0) sprintf(text2, "    *ACCESS DENIED*");
    return text2;
}

int List(const char *sDir)
{
    WIN32_FIND_DATA file;
    HANDLE hFind = NULL;
    char sPath[2048];
    unsigned long long rootsize, rootfiles;
    rootfiles = 0;
    rootsize = 0;
    if ((dirnumber == 0) && (filenumber == 0)) 
    {
        filedir = 0;
        dirsize = 0;
    }
    sprintf(sPath, "%s\\*.*", sDir);
    access = 1;
    if ((hFind = FindFirstFile(sPath, &file)) == INVALID_HANDLE_VALUE)
    {
        access = 0;
    }
    do
    {   
        sprintf(sPath, "%s\\%s", sDir, file.cFileName);
        //FindFirstFile will return "." and ".." as the first two dirs.
        if (strcmp(file.cFileName, ".") != 0 && strcmp(file.cFileName, "..") != 0)
        {
            //Is it a File or Dir?
            if (file.dwFileAttributes &FILE_ATTRIBUTE_DIRECTORY)
            {
                dirnumber++;
                level++;
                List(sPath);
                level--;
            } else {
                filesize = file.nFileSizeHigh * (MAXDWORD+1) + file.nFileSizeLow;
                rootfiles++;
                rootsize = rootsize + filesize;
                if ((level < limit - 1) && (limit > 1) && (dironly == 0))
                {
                    if (filesize >= minsize)
                        printf("%20s  FILE       1   %s\n", format(filesize), sPath);
                } 
            }
        }
    }
    while(FindNextFile(hFind, &file)); 
    FindClose(hFind); 
    size = size + rootsize;
    filenumber = filenumber + rootfiles; 
    dirsize = dirsize + rootsize;
    filedir = filedir + rootfiles;
    if ((level < limit) && (limit > 1))
    {
        if (dirsize >= minsize) 
            if ((level < limit - 1) && (dironly ==0))
            {
                printf("%20s  -DIR  %6llu   %s\n", format(dirsize), filedir, sDir);
            } else {
                printf("%20s  +DIR  %6llu   %s\n", format(dirsize), filedir, sDir);
            }
        filedir = 0;
        dirsize = 0;
    } 

    return 0;
}

int main(int argc, char *argv[])
{
size = 0;
filenumber = 0;
dirnumber = 0;
level = 0;
limit = 1;
dir = ".";
dironly = 0;
minsize = 0;
access = 1;

for (i = 1; i < argc; ++i)
{
    if ((!strcmp(argv[i], "help")) || (!strcmp(argv[i], "help"))) 
    {
        printf("\n DIRCALC 1.0 - Created by Shpati Koleka, MMXXI - MIT License.\n");
        printf(" DIRCALC displays the disk usage in bytes for any given directory.\n\n");
        printf(" Usage: DIRCALC [/D] [/S size] [/L levels] [/A] [directory_location]\n\n");
        printf(" /D       Displays only the sizes of the DIRs, ommiting the FILEs.\n");
        printf(" /S size  Displays only FILEs and DIRs whose byte-size is = or > than given.\n");
        printf(" /L level Displays the FILE and DIR sizes for the given level. \n");
        printf(" /A       Displays all files at all levels. \n");
        printf(" HELP     Displays the help, i.e. this page.\n\n");
        printf(" Examples:\n\n");
        printf(" dircalc \n"); 
        printf(" dircalc C:\\Users\\Public\\Desktop \n");  
        printf(" dircalc /L 10 %%temp%% \n"); 
        printf(" dircalc /D \\\\REMOTE\\Folder\\ \n");  
        printf(" dircalc /S 1000000 \n"); 
        printf(" dircalc /A \n"); 
        return 0;
    } else if ((!strcmp(argv[i], "/D")) || (!strcmp(argv[i], "/d"))) {
        dironly = 1; 
    } else if ((!strcmp(argv[i], "/S")) || (!strcmp(argv[i], "/s"))) {
        minsize = atoi(argv[i+1]);
        i++;
    } else if ((!strcmp(argv[i], "/L")) || (!strcmp(argv[i], "/l"))) {
        limit = atoi(argv[i+1]); 
        i++;
    } else if ((!strcmp(argv[i], "/A")) || (!strcmp(argv[i], "/a"))) {
        limit = 3000; 
    } else {
        dir = argv[i];
    }
}
i = 0;

if (limit != 0) printf("\n            Size (b)  Type   Files   Name\n");
limit++;
if (dironly == 1)
{
    List(dir);
} else {
    List(dir);
}

n = size;
i = 0;
while (n > 1024) 
{
   n = n / 1024; 
   i++;
}
printf("\n%20s bytes",format(size));
if ((i > 0) && (i < 9)) printf(" (%.2f %cB)", n, s[i-1]);
printf(" in %llu FILEs within %llu DIRs\n", filenumber, dirnumber);
return 0;
}