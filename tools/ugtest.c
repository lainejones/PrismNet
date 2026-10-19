/*
 * ugtest - PrismNet usergroup.library test client.
 * GPL-3.0-only, see LICENSE.
 *
 *   ugtest [USER=expected-login] [DBUSER=name DBUID=n]
 *
 * USER   : the login the library should report (default user from ENV:USER)
 * DBUSER : a user that must exist in ENV:PrismNet/passwd, with uid DBUID
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/usergroup.h>
/* Only the inline stubs: clib/usergroup_protos.h clashes with libnix's grp.h. */
struct passwd;
struct group;
/* libnix has its own internal inline/usergroup.h that shadows the NDK one */
#include <../ndk-include/inline/usergroup.h>

/* AmiTCP layouts, as returned by usergroup.library */
struct ug_passwd { char *pw_name, *pw_passwd; LONG pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; };
struct ug_group  { char *gr_name, *gr_passwd; LONG gr_gid; char **gr_mem; };

struct Library *UserGroupBase;
static LONG my_errno;
static int passes, fails;

#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("  FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    LONG args[3] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"USER/K,DBUSER/K,DBUID/K/N", args, NULL);
    struct ug_passwd *pw;
    struct ug_group *gr;
    char login[40], c1[16];
    char *s;
    LONG gids[8], n;

    UserGroupBase = OpenLibrary((CONST_STRPTR)"usergroup.library", 4);
    if (!UserGroupBase) {
        printf("ugtest: cannot open usergroup.library 4\n");
        return RETURN_FAIL;
    }
    printf("ugtest: %s\n", (char *)UserGroupBase->lib_IdString);

    {
        struct TagItem tags[] = { { UGT_ERRNOLPTR, (ULONG)&my_errno }, { TAG_END, 0 } };
        CHECK(ug_SetupContextTagList((STRPTR)"ugtest", tags) == 0, "ug_SetupContextTagList");
    }

    s = getlogin();
    CHECK(s != NULL, "getlogin");
    strncpy(login, s ? s : "", sizeof(login) - 1);
    login[sizeof(login) - 1] = 0;
    printf("  login '%s' uid %ld gid %ld euid %ld egid %ld\n", login, (long)getuid(),
           (long)getgid(), (long)geteuid(), (long)getegid());
    if (args[0])
        CHECK(!strcmp(login, (char *)args[0]), "login is '%s', expected '%s'", login, (char *)args[0]);

    pw = (struct ug_passwd *)getpwuid(getuid());
    CHECK(pw != NULL, "getpwuid(getuid())");
    if (pw) {
        printf("  passwd: %s:%s:%ld:%ld:%s:%s:%s\n", pw->pw_name, pw->pw_passwd, (long)pw->pw_uid,
               (long)pw->pw_gid, pw->pw_gecos, pw->pw_dir, pw->pw_shell);
        CHECK(!strcmp(pw->pw_name, login), "getpwuid name matches login");
    }
    pw = (struct ug_passwd *)getpwnam((STRPTR)login);
    CHECK(pw && pw->pw_uid == getuid(), "getpwnam(login)");
    my_errno = 0;
    CHECK(getpwnam((STRPTR)"no-such-user") == NULL, "getpwnam(unknown) == NULL");
    CHECK(my_errno != 0, "errno set via UGT_ERRNOLPTR");

    gr = (struct ug_group *)getgrgid(getgid());
    CHECK(gr != NULL, "getgrgid(getgid())");
    if (gr)
        printf("  group: %s gid %ld first member %s\n", gr->gr_name, (long)gr->gr_gid,
               gr->gr_mem && gr->gr_mem[0] ? gr->gr_mem[0] : "(none)");
    n = getgroups(8, gids);
    CHECK(n >= 1 && gids[0] == getgid(), "getgroups");

    n = 0;
    setpwent();
    while (getpwent() && n < 100) n++;
    endpwent();
    CHECK(n >= 1, "getpwent enumerates (%ld entries)", (long)n);
    printf("  passwd entries: %ld\n", (long)n);

    CHECK(umask(077) == 022, "umask default 022");
    CHECK(getumask() == 077, "getumask after umask(077)");

    s = crypt((STRPTR)"secret", (STRPTR)"ab");
    CHECK(s && strlen(s) == 13 && s[0] == 'a' && s[1] == 'b', "crypt format");
    if (s) {
        strcpy(c1, s);
        printf("  crypt(secret, ab) = %s\n", c1);
        s = crypt((STRPTR)"secret", (STRPTR)"ab");
        CHECK(s && !strcmp(s, c1), "crypt deterministic");
        s = crypt((STRPTR)"Secret", (STRPTR)"ab");
        CHECK(s && strcmp(s, c1), "crypt depends on key");
    }

    if (args[1]) {
        pw = (struct ug_passwd *)getpwnam((STRPTR)args[1]);
        CHECK(pw != NULL, "database user %s exists", (char *)args[1]);
        if (pw && args[2])
            CHECK(pw->pw_uid == *(LONG *)args[2], "%s has uid %ld (got %ld)", (char *)args[1],
                  (long)*(LONG *)args[2], (long)pw->pw_uid);
        if (pw && args[2]) {
            pw = (struct ug_passwd *)getpwuid(*(LONG *)args[2]);
            CHECK(pw && !strcmp(pw->pw_name, (char *)args[1]), "getpwuid(%ld) from database",
                  (long)*(LONG *)args[2]);
        }
    }

    CHECK(setuid(500) == 0 && getuid() == 500, "setuid");

    CloseLibrary(UserGroupBase);
    if (rda) FreeArgs(rda);
    printf("ugtest: %d passed, %d failed -> %s\n", passes, fails, fails ? "FAIL" : "PASS");
    return fails ? RETURN_WARN : RETURN_OK;
}
