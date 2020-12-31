/***************************************************************************
                          sftp.cpp  -  description
                             -------------------
    begin                : Fri Jun 29 23:45:40 CDT 2001
    copyright            : (C) 2001 by Lucas Fisher
    email                : ljfisher@iastate.edu
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

/*
- add dialog to ask for username
- rename() causes SSH to die
- How to handle overwrite?
- After the user cancels with the stop button, we get ERR_CANNOT_LAUNCH_PROCESS
  errors, until we kill the ioslave. Same thing after trying the wrong passwd
  too many times.
  This is happening because KProcess thinks that the ssh process is still running
  even though it exited.
- How to handle password and caching?
  - Write our own askpass program using kde
  - set env SSH_ASKPASS_PROGRAM before launching
    -how to do this? KProcess doesn't give us access to env variables.
  - Our askpass program can probably talk to the kdesu daemon to implement caching.
- chmod() succeeds, but konqueror always puts permissions to 0 afterwards. The properties
  dialog is right though.
  Nevermind - ftp ioslave does this too! Maybe a bug with konqueror.
- stat does not give us group and owner names, only numbers.  We could cache the uid/name and
  gid/name so we can give names when doing a stat also.
7-13-2001 - ReadLink stopped working. sftp server always retuns a file not found error
          - Need to implement 64 bit file lengths-->write DataStream << for u_int64
            Still need to offer 32 bit size since this is what kde wants. ljf
          - rename() isn't exactly causing ioslave to die.  The stat of the file we are
            going to rename is killing the slave.  The slave dies in the statEntry() call.
            I don't know what I am putting in the UDS entry that is causing this. ljf
7-14-2001 - got put, mimetype working ljf
          - fixed readlink problem - I was sending the wrong path. doh! ljf
7-17-2001 - If the user changes the host, the slave doesn't change host! setHost() is not
            called, nor is another ioslave spawned. I have not investigated the problem
            yet. ljf

DEBUGGING
We are pretty much left with kdDebug messages for debugging. We can't use a gdb
as described in the ioslave DEBUG.howto because kdeinit has to run in a terminal.
Ssh will detect this terminal and ask for a password there, but will just get garbage.
So we can't connect.
*/



#include <qcstring.h>
#include <qstring.h>
#include <qdatastream.h>
#include <qobject.h>
#include <qstrlist.h>

#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <kapp.h>
#include <kdebug.h>
#include <kmessagebox.h>
#include <kinstance.h>
#include <kglobal.h>
#include <kstddirs.h>
#include <klocale.h>
#include <kurl.h>
#include <kdebug.h>
#include <kstddirs.h>

#include "sftp.h"
#include "kio_sftp.h"
#include "kprocessblockingrw.h"
#include "sftpfileattr.h"

#define MIN(a,b) (((a)<(b))?(a):(b))

using namespace KIO;
extern "C"
{
  int kdemain( int argc, char **argv )
  {
    KInstance instance( "kio_sftp" );
    
    kdDebug(KIO_SFTP_DB) << "*** Starting kio_sftp " << endl;

    if (argc != 4)
      {
	kdDebug(KIO_SFTP_DB) << "Usage: kio_sftp  protocol domain-socket1 domain-socket2" << endl;
	exit(-1);
      }

    kio_sftpProtocol slave(argv[2], argv[3]);
    slave.dispatchLoop();

    kdDebug(KIO_SFTP_DB) << "*** kio_sftp Done" << endl;
    return 0;
  }
}

kio_sftpProtocol::kio_sftpProtocol(const QCString &pool_socket, const QCString &app_socket)
  : QObject(), SlaveBase("kio_sftp", pool_socket, app_socket)
{
 kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::kio_sftpProtocol()" << endl;
// okWriteStdin = true;
 mConnected = false;
 mMsgId = 0;
}
/* ---------------------------------------------------------------------------------- */


kio_sftpProtocol::~kio_sftpProtocol()
{
  kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::~kio_sftpProtocol()" << endl;
}



void kio_sftpProtocol::get(const KURL& url ) {
    kdDebug(KIO_SFTP_DB) << "kio_sftp::get(const KURL& url)" << endl ;
    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_UNKNOWN, QString::null);
            finished();
            return;
        }
    }

    int code;
    sftpFileAttr attr;
    // stat the file first to get its size
    if( (code = sftpStat(url, attr)) != SSH2_FX_OK ) {
        processStatus(code, url.prettyURL());
        return;
    }
    totalSize(attr.fileSize());


    Q_UINT32 pflags = SSH2_FXF_READ;
    QByteArray handle, mydata;
    attr.clear();
    if( (code = sftpOpen(url, pflags, attr, handle)) != SSH2_FX_OK ) {
        error(ERR_CANNOT_OPEN_FOR_READING, url.prettyURL());
        return;
    }

    // How big should each data packet be? Large gives better tranfer rate
    // over high speed connections, low probably better for modems.
    Q_UINT32 len = 8*1024;
    Q_UINT32 offset = 0;
    time_t now, start = time(NULL), last = start;
    code = SSH2_FX_OK;
    while( code == SSH2_FX_OK ) {
        if( (code = sftpRead(handle, offset, len, mydata)) == SSH2_FX_OK ) {
            data(mydata);
            offset += mydata.size();
            processedSize(offset);
            now = time(NULL);
            if( now - last > 1 ) {
                speed(offset / (now - start));
                last = now;
            }
            kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::get(): offset = " << offset << endl;
        }
    }

    if( code != SSH2_FX_EOF ) {
        error(ERR_COULD_NOT_READ, url.prettyURL());
        return; // return here or still send empty array to indicate end of read?
    }

    data(QByteArray());
    processedSize(offset);
    sftpClose(handle);
    finished();
}


/** No descriptions */
void kio_sftpProtocol::setHost (const QString& h, int port, const QString& user, const QString& pass){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::setHost() " << user << "@" << h << ":" << port << endl;

    mHost = h;
    if( port <= 0 )
        port = 22;

    mPort = port;
    mUsername = user;
}

/** No descriptions */
void kio_sftpProtocol::openConnection(){
    kdDebug(KIO_SFTP_DB) << "openConnection() to " << mUsername << "@" << mHost << ":" << mPort << endl;

    if(mConnected) return;

    infoMessage(i18n("Opening connection to host <b>%1:%2</b>").arg(mHost).arg(mPort));

    if( mHost.isEmpty() ) {
        kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::openConnection Need hostname" << endl;
        error(ERR_UNKNOWN_HOST, QString::null);
        return;
    }

    if( !startSsh() ) {
        mConnected = false;
        return;
    }

    kdDebug(KIO_SFTP_DB) << "Sending SSH2_FXP_INIT packet." << endl;
    QByteArray p;
    QDataStream packet(p, IO_WriteOnly);
    packet << (Q_UINT32)5;                     // packet length
    packet << (Q_UINT8) SSH2_FXP_INIT;         // packet type
    packet << (Q_UINT32)SSH2_FILEXFER_VERSION; // client version

    putPacket(p);
    getPacket(p);

    QDataStream s(p, IO_ReadOnly);
    Q_UINT32 version;
    Q_UINT8  type;
    s >> type;
    kdDebug(KIO_SFTP_DB) << "Got type " << type << endl;
    if( type == SSH2_FXP_VERSION ) {
        s >> version;
        kdDebug(KIO_SFTP_DB) << "Got server version " << version << endl;
        // XXX Get extensions here
        if( version != SSH2_FILEXFER_VERSION ) {
            error(ERR_UNSUPPORTED_PROTOCOL, "Server uses incompatible sftp version.");
            closeConnection();
            return;
        }
    }
    else {
        error(ERR_UNKNOWN, "Protocol error.");
        closeConnection();
        return;
    }

    mConnected = true;
    connected();
    return;
}

/** No descriptions */
void kio_sftpProtocol::closeConnection() {
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::closeConnection()" << endl;
    ssh.kill();
    mConnected = false;
}

/** No descriptions */
void kio_sftpProtocol::put ( const KURL& url, int permissions, bool overwrite, bool resume ){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::put()" << endl;
    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    int code;
    sftpFileAttr attr;
    // Stat file to see if it already exists
    bool alreadyExists = false;
    if( (code = sftpStat(url, attr)) == SSH2_FX_OK ) {
        alreadyExists = true;
    }
    else if( code != SSH2_FX_NO_SUCH_FILE ) {
        processStatus(code, url.prettyURL());
        return;
    }

    Q_UINT32 pflags;
    if( overwrite && !resume )
        pflags = SSH2_FXF_WRITE | SSH2_FXF_CREAT | SSH2_FXF_TRUNC;
    else if( !overwrite && !resume )
        pflags = SSH2_FXF_WRITE | SSH2_FXF_CREAT | SSH2_FXF_EXCL;
    else {
        error(ERR_UNSUPPORTED_ACTION, "Resume");
        return;
    }
//    else if( overwrite && resume )
//        pflags = SSH2_FXF_WRITE;
//    else if( !overwrite && resume )
//        pflags = SSH2_FXF_WRITE | SSH2_FXF_APPEND;

    attr.clear();
    if( !alreadyExists )
        attr.setPermissions(permissions);

    QByteArray handle;
    code = sftpOpen(url, pflags, attr, handle);
    if( code == SSH2_FX_FAILURE ) { // assume failure means file exists
        error(ERR_FILE_ALREADY_EXIST, url.prettyURL());
        return;
    }
    else if( code != SSH2_FX_OK ) {
        processStatus(code, url.prettyURL());
        return;
    }

    // How big should each data packet be? Large gives better tranfer rate
    // over high speed connections, low probably better for modems.
    Q_UINT32 len = 8*1024;
    Q_UINT32 offset = 0;
    int nbytes;
    time_t now, start = time(NULL), last = start;
    QByteArray mydata;
    dataReq();
    nbytes = readData(mydata);
    while( nbytes > 0 ) {
        if( (code = sftpWrite(handle, offset, mydata)) != SSH2_FX_OK ) {
            error(ERR_COULD_NOT_WRITE, url.prettyURL());
            return;
        }

        offset += mydata.size();
        processedSize(offset);

        now = time(NULL);
        if( now - last > 1 ) {
            speed(offset / (now - start));
            last = now;
        }
        kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::put(): offset = " << offset << endl;

        dataReq();
        nbytes = readData(mydata);
    }

    if( nbytes < 0 ) {
        error(ERR_COULD_NOT_WRITE, url.prettyURL());
    }

    sftpClose(handle);
    finished();
}

/** No descriptions */
void kio_sftpProtocol::stat ( const KURL& url ){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::stat( " << url.prettyURL() << " )" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    if( !url.hasPath() ) {
        KURL newUrl, oldUrl;
        newUrl = oldUrl = url;
        oldUrl.setPath(QString::fromLatin1("."));
        if( sftpRealPath(oldUrl, newUrl) == SSH2_FX_OK ) {
            kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::stat: Redirecting to " << newUrl.prettyURL() << endl;
            redirection(newUrl);
            finished();
            return;
        }
    }

    int code;
    sftpFileAttr attr;
    if( (code = sftpStat(url, attr)) != SSH2_FX_OK )
        processStatus(code, url.prettyURL());
    else {
        kdDebug() << "We sent and received stat packet ok" << endl;
        attr.setFilename(url.filename());
        // dies here when stating file for rename
        UDSEntry e;
        UDSAtom a;
        a.m_uds = UDS_NAME;
        a.m_str = attr.filename();
        e.append(a);
        a.m_uds = UDS_SIZE;
        a.m_long = attr.fileSize();
        e.append(a);
        statEntry(e);
    }
    finished();
    kdDebug() << "End of kio_sftpProtocol::stat()" << endl;
    return;
}

/** No descriptions */
void kio_sftpProtocol::mimetype ( const KURL& url ){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::mimetype( " << url.prettyURL() << " )" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    Q_UINT32 pflags = SSH2_FXF_READ;
    QByteArray handle, mydata;
    sftpFileAttr attr;
    int code;
    if( (code = sftpOpen(url, pflags, attr, handle)) != SSH2_FX_OK ) {
        error(ERR_CANNOT_OPEN_FOR_READING, url.prettyURL());
        return;
    }

    Q_UINT32 len = 1024; // Get first 1k for determining mimetype
    Q_UINT32 offset = 0;
    time_t now, start = time(NULL), last = start;
    code = SSH2_FX_OK;
    while( offset < len && code == SSH2_FX_OK ) {
        if( (code = sftpRead(handle, offset, len, mydata)) == SSH2_FX_OK ) {
            data(mydata);
            offset += mydata.size();
            processedSize(offset);
            now = time(NULL);
            if( now - last > 1 ) {
                speed(offset / (now - start));
                last = now;
            }
            kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::mimetype(): offset = " << offset << endl;
        }
    }


    data(QByteArray());
    processedSize(offset);
    sftpClose(handle);
    finished();
}

/** No descriptions */
void kio_sftpProtocol::listDir(const KURL& url) {
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::listDir(" << url.prettyURL() << ")" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    if( !url.hasPath() ) {
        KURL newUrl, oldUrl;
        newUrl = oldUrl = url;
        oldUrl.setPath(QString::fromLatin1("."));
        if( sftpRealPath(oldUrl, newUrl) == SSH2_FX_OK ) {
            kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::listDir: Redirecting to " << newUrl.prettyURL() << endl;
            redirection(newUrl);
            finished();
            return;
        }
    }
    QByteArray handle;
    QString path = url.path();
    int code;

    if( (code = sftpOpenDirectory(url, handle)) != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::listDir(): open directory failed" << endl;
        processStatus(code, url.prettyURL());
        return;
    }


    code = SSH2_FX_OK;
    while( code == SSH2_FX_OK ) {
        code = sftpReadDir(handle, url);
        if( code != SSH2_FX_OK && code != SSH2_FX_EOF )
            processStatus(code, url.prettyURL());
        kdDebug() << "sftpReadDir return code " << code ;
    }

    if( (code = sftpClose(handle)) != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::listdir(): closing of directory failed" << endl;
        processStatus(code, url.prettyURL());
        return;
    }

    finished();
}

/** Make a directory.
    OpenSSH does not follow the internet draft for sftp in this case.
    The format of the mkdir request expected by OpenSSH sftp server is:
        uint32 id
        string path
        ATTR   attr
 */
void kio_sftpProtocol::mkdir(const KURL&url, int permissions){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::mkdir()" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    QString path = url.path();
    Q_UINT32 id, expectedId;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    sftpFileAttr attr;
    attr.setPermissions(permissions);

    id = expectedId = mMsgId++;
    kdDebug(KIO_SFTP_DB) << "creating dir " << path << endl;
    s << Q_UINT32(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + path.length() + attr.size());
    s << (Q_UINT8)SSH2_FXP_MKDIR;
    s << id;
    s.writeBytes(path.latin1(), path.length());
    s << attr;
    kdDebug() << "mkdir(): packet size is " << p.size() << endl;

    putPacket(p);
    getPacket(p);

    Q_UINT8 type;
    QDataStream r(p, IO_ReadOnly);

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::mkdir: sftp packet id mismatch" << endl;
        error(ERR_COULD_NOT_MKDIR, path);
        finished();
        return;
    }

    if( type != SSH2_FXP_STATUS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::mkdir(): unexpected packet type of " << type << endl;
        error(ERR_COULD_NOT_MKDIR, path);
        finished();
        return;
    }

    int code;
    r >> code;
    if( code != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::mkdir(): failed with code " << code << endl;
        error(ERR_COULD_NOT_MKDIR, path);
    }

//    sftpFileAttr attr;
//    attr.setPermissions(permissions);
//    if( (code = sftpSetStat(url, attr)) != SSH2_FX_OK ) {
//        processStatus(code);
//    }

    finished();
}

/** No descriptions */
void kio_sftpProtocol::rename(const KURL& src, const KURL& dest, bool overwrite){
    kdError(KIO_SFTP_DB) << "kio_sftpProtocol::rename(" << src.prettyURL() << ", " << dest.prettyURL() << ")" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    int code;
    bool failed = false;
    if( (code = sftpRename(src, dest)) != SSH2_FX_OK ) {
        if( overwrite ) { // try to delete the destination
            sftpFileAttr attr;
            if( (code = sftpStat(dest, attr)) != SSH2_FX_OK ) {
                failed = true;
            }
            else {
                if( (code = sftpRemove(dest, !S_ISDIR(attr.permissions())) ) != SSH2_FX_OK ) {
                    failed = true;
                }
                else {
                    // XXX what if rename fails again? We have lost the file.
                    // Maybe rename dest to a temporary name first? If rename is
                    // successful, then delete?
                    if( (code = sftpRename(src, dest)) != SSH2_FX_OK )
                        failed = true;
                }
            }
        }
        else if( code == SSH2_FX_FAILURE ) {
            error(ERR_FILE_ALREADY_EXIST, dest.prettyURL() );
            return;
        }
        else
            failed = true;
   }

    // What error code do we return? Code for the original symlink command
    // or for the last command or for both? The second one is implemented here.
    if( failed )
        processStatus(code);

    finished();
}

/** No descriptions */
void kio_sftpProtocol::symlink(const QString& target, const KURL& dest, bool overwrite){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::symlink()" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    int code;
    bool failed = false;
    if( (code = sftpSymLink(target, dest)) != SSH2_FX_OK ) {
        if( overwrite ) { // try to delete the destination
            sftpFileAttr attr;
            if( (code = sftpStat(dest, attr)) != SSH2_FX_OK ) {
                failed = true;
            }
            else {
                if( (code = sftpRemove(dest, !S_ISDIR(attr.permissions())) ) != SSH2_FX_OK ) {
                    failed = true;
                }
                else {
                    // XXX what if rename fails again? We have lost the file.
                    // Maybe rename dest to a temporary name first? If rename is
                    // successful, then delete?
                    if( (code = sftpSymLink(target, dest)) != SSH2_FX_OK )
                        failed = true;
                }
            }
        }
        else if( code == SSH2_FX_FAILURE ) {
            error(ERR_FILE_ALREADY_EXIST, dest.prettyURL());
            return;
        }
        else
            failed = true;
    }

    // What error code do we return? Code for the original symlink command
    // or for the last command or for both? The second one is implemented here.
    if( failed )
        processStatus(code);

    finished();
}

/** No descriptions */
void kio_sftpProtocol::chmod(const KURL& url, int permissions){
    QString perms; perms.setNum(permissions, 8);
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::chmod(" << url.prettyURL() << ", " << perms << ")" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            finished();
            return;
        }
    }

    sftpFileAttr attr;
    attr.setPermissions(permissions);
    int code;
    if( (code = sftpSetStat(url, attr)) != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::chmod(): sftpSetStat failed with error " << code << endl;
        if( code == SSH2_FX_FAILURE )
            error(ERR_CANNOT_CHMOD, QString::null);
        else
            processStatus(code, url.prettyURL());
    }
    finished();
}

/** No descriptions */
void kio_sftpProtocol::copy(const KURL &src, const KURL &dest, int permissions, bool overwrite) {
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::copy()" << endl;
    error(ERR_UNSUPPORTED_ACTION, QString::null);
    finished();
}

/** No descriptions */
void kio_sftpProtocol::del(const KURL &url, bool isfile){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::del(" << url.prettyURL() << ", " << (isfile?"file":"dir") << ")" << endl;

    if( !mConnected ) {
        openConnection();
        if( !mConnected ) {
            error(ERR_COULD_NOT_CONNECT, QString::null);
            return;
        }
    }

    int code;
    if( (code = sftpRemove(url, isfile)) != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::del(): sftpRemove failed with error code " << code << endl;
        processStatus(code, url.prettyURL());
    }
    finished();
}

/** No descriptions */
void kio_sftpProtocol::slave_status(){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::slave_status(): " << (mConnected ? "" : "not") << " connected to " << mHost << endl;
    slaveStatus(mConnected ? mHost : QString::null, mConnected);
}

/** No descriptions */
void kio_sftpProtocol::reparseConfiguration(){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::reparseConfiguration()" << endl;
}

/** Connect to SlaveBase::ProcessExited.
Sets the connected flag to false. */
void kio_sftpProtocol::slotSshExited(KProcess * p){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::slotSshExited()" << endl;
    mConnected = false;
}

bool kio_sftpProtocol::getPacket(QByteArray& msg) {
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::getPacket()" << endl;
    int len;
    unsigned int msgLen;
    char buf[4096];

    // Get the message length and type
    len = ssh.readStdout(buf, 4, true /*wait all*/);
    if( len == 0 ) {
        error( ERR_CONNECTION_BROKEN, mHost);
        return false;
    }
    else if( len == -1 ) {
        error( ERR_CONNECTION_BROKEN, mHost);
        return false;
    }
    QByteArray a;
    a.duplicate(buf, (unsigned int)4);
    QDataStream s(a, IO_ReadOnly);
    s >> msgLen;
//    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::getPacket(): Got msg length of " << msgLen << endl;
    if( !msg.resize(msgLen) ) {
        error( ERR_OUT_OF_MEMORY, "Could not allocate memory for sftp packet.");
        return false;
    }

    unsigned int offset = 0;
    while( msgLen ) {
        len = ssh.readStdout(buf, MIN(msgLen, sizeof(buf)), true /*wait all*/);
        if( len == 0 ) {
            error(ERR_CONNECTION_BROKEN, "Connection closed");
            return false;
        }
        else if( len == -1 ) {
            error(ERR_CONNECTION_BROKEN, "Couldn't read sftp packet");
            return false;
        }
        msgLen -= len;
        mymemcpy(buf, msg, offset, len);
        offset += len;
    }
//    kdDebug(KIO_SFTP_DB) << "Got packet (" << msg.size() << "): [" << msg << "]" << endl;
    return true;
}

/** Send an sftp packet to stdin of the ssh process. */
bool kio_sftpProtocol::putPacket(QByteArray& p){
    kdDebug(KIO_SFTP_DB) << "kiosftp_Protocol::putPacket(): size == " << p.size() << endl;
//    kdDebug(KIO_SFTP_DB) << "sending packet (" << p.size() << "): [" << p << "]" << endl;
    int ret = ssh.writeStdin(p.data(), p.size(), true /*blocking*/, true /*wait all*/);

    if( ret <= 0 )
        return false;

    return true;
}

bool kio_sftpProtocol::startSsh() {
    QString sshPath;
    sshPath = KStandardDirs::findExe(QString::fromLatin1("ssh"));
    if( sshPath.isEmpty() ) {
        kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::startSsh(): ssh path is empty" << endl;
        error(ERR_CANNOT_LAUNCH_PROCESS, sshPath);
        return false;
    }

    ssh.clearArguments();
//    ssh << sshPath;
    ssh << "/usr/local/bin/ssh";
    ssh << "-v" << "-v" << "-v";
    ssh << "-l" << mUsername;
    ssh << "-s";
    ssh << "-oForwardX11=no";
    ssh << "-oForwardAgent=no";
    ssh << "-oProtocol=2";
    ssh << mHost;
    ssh << "sftp";
/*
    char *x;
    QStrList* list = ssh.args();
    x = list->first();
    while( x != NULL  ) {
        kdDebug(KIO_SFTP_DB) << "cmd line arg: " << x << endl;
        x = list->next();
    }
*/
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::startSsh(): ssh is " << (ssh.isRunning()?"":"not") << " running" << endl;

    // XXX Connect signals
    if( !QObject::connect(&ssh, SIGNAL(processExited(KProcess *)),
                     this, SLOT(slotSshExited(KProcess *))) ) {
        kdDebug(KIO_SFTP_DB) << "connect processExited to slotSshExited failed" << endl;
    }
#if 0
    if( !QObject::connect(&ssh, SIGNAL(receivedStdout (int, int &)),
                     this, SLOT(slotReceivedStdout(int, int&))) ) {
        kdDebug(KIO_SFTP_DB) << "connect receivedStdout to slotReceivedStdout failed" << endl;
    }
    if( !QObject::connect(&ssh, SIGNAL(receivedStdout (KProcess *, char *, int)),
                     this, SLOT(slotReceivedStdout(KProcess *, char *, int))) ) {
        kdDebug(KIO_SFTP_DB) << "connect receivedStdout(buf) to slotReceivedStdout(buf) failed" << endl;
    }
    if( !QObject::connect(&ssh, SIGNAL(receivedStderr (KProcess *, char *, int)),
                     this, SLOT(slotReceivedStderr(KProcess *, char*, int))) ) {
        kdDebug(KIO_SFTP_DB) << "connect receivedStderr to slotReceivedStderr failed" << endl;
    }
    if( !QObject::connect(&ssh, SIGNAL(wroteStdin(KProcess *)),
                          this, SLOT(wroteStdinDone(KProcess *))) ) {
        kdDebug(KIO_SFTP_DB) << "connect wroteStdin to slotWroteStdinDone failed" << endl;
    }
#endif
    if( !ssh.start(KProcess::NotifyOnExit, KProcess::All) ){
        kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::startSsh(): start failed" << endl;
        error(ERR_CANNOT_LAUNCH_PROCESS, sshPath);
        return false;
    }

    return true;
}

/** Used to have the server canonicalize any given path name to an absolute path.
This is useful for converting path names containing ".." components or relative
pathnames without a leading slash into absolute paths.
Returns the canonicalized url. */
int kio_sftpProtocol::sftpRealPath(const KURL& url, KURL& newUrl){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRealPath(" << url.prettyURL() << ", newUrl)" << endl;
    QString path = url.path();
    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    s << Q_UINT32(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + path.length());
    s << (Q_UINT8)SSH2_FXP_REALPATH;
    s << id;
    s.writeBytes(path.latin1(), path.length());

    putPacket(p);
    getPacket(p);

    Q_UINT8 type;
    QDataStream r(p, IO_ReadOnly);

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRealPath: sftp packet id mismatch" << endl;
        return -1;
    }

    if( type == SSH2_FXP_STATUS ) {
        Q_UINT32 code;
        r >> code;
        return code;
    }

    if( type != SSH2_FXP_NAME ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRealPath(): unexpected packet type of " << type << endl;
        return -1;
    }

    Q_UINT32 count;
    r >> count;
    if( count != 1 ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRealPath(): Bad number of file attributes for realpath command" << endl;
        return -1;
    }

    QCString newPath;
    r >> newPath;
    // Make sure there is a terminating null character. QCString gets the string size
    // but I don't think a null character is appended. += doesn't always seem to work.
    int len = newPath.size();
    newPath.resize(newPath.size()+1);
    newPath[len] = '\0';
    newUrl.setPath(newPath);
    return SSH2_FX_OK;
}

/** Process SSH_FXP_STATUS packets. */
void kio_sftpProtocol::processStatus(Q_UINT8 code, QString message){
    switch(code) {
    case SSH2_FX_OK:
        break;
    case SSH2_FX_EOF:
        break;
    case SSH2_FX_NO_SUCH_FILE:
        error(ERR_DOES_NOT_EXIST, message);
        break;
    case SSH2_FX_PERMISSION_DENIED:
        error(ERR_ACCESS_DENIED, message);
        break;
    case SSH2_FX_FAILURE:
        error(ERR_UNKNOWN, "Sftp command failed.");
        break;
    case SSH2_FX_BAD_MESSAGE:
        error(ERR_UNKNOWN, "Bad message.");
        break;
    case SSH2_FX_OP_UNSUPPORTED:
        error(ERR_UNKNOWN, "Unsupported op.");
//    Should never be returned by server
//    case SSH2_FX_NO_CONNECTION:
//    case SSH2_FX_CONNECTION_LOST:
    default:
        QString msg = "error code: ";
        QString x; x.setNum(code);
        msg += x;
        msg.arg(code);
        error(ERR_UNKNOWN, msg);
    }
}

/** Opens a directory handle for url.path. Returns true if succeeds. */
int kio_sftpProtocol::sftpOpenDirectory(const KURL& url, QByteArray& handle){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpenDirectory(" << url.prettyURL() << ", handle)" << endl;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);
    QString path = url.path();

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + path.length());
    s << (Q_UINT8)SSH2_FXP_OPENDIR;
    s << (Q_UINT32)id;
    s.writeBytes(path.latin1(), path.length());

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpenDirectory: sftp packet id mismatch" << endl;
        return -1;
    }

    if( type == SSH2_FXP_STATUS ) {
        Q_UINT32 errCode;
        r >> errCode;
        return errCode;
    }

    if( type != SSH2_FXP_HANDLE ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpenDirectory: unexpected message type of " << type << endl;
        return -1;
    }

    r >> handle;
    if( handle.size() > 256 ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpenDirectory: handle exceeds max length" << endl;
        return -1;
    }

    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpenDirectory: handle (" << handle.size() << "): [" << handle << "]" << endl;
    return SSH2_FX_OK;
}

/** Closes a directory or file handle. */
int kio_sftpProtocol::sftpClose(const QByteArray& handle){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpClose()" << endl;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + handle.size());
    s << (Q_UINT8)SSH2_FXP_CLOSE;
    s << (Q_UINT32)id;
    s << handle;

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpClose: sftp packet id mismatch" << endl;
        return -1;
    }

    if( type != SSH2_FXP_STATUS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpClose: unexpected message type of " << type << endl;
        return -1;
    }

    Q_UINT32 code;
    r >> code;
    if( code != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpClose: close failed with err code " << code << endl;
    }

    return code;
}

/** Set a files attributes. */
int kio_sftpProtocol::sftpSetStat(const KURL& url, const sftpFileAttr& attr){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSetStat(" << url.prettyURL() << ", attr)" << endl;
    QString path = url.path();
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + path.length() + attr.size());
    s << (Q_UINT8)SSH2_FXP_SETSTAT;
    s << (Q_UINT32)id;
    s.writeBytes(path.latin1(), path.length());
    s << attr;

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSetStat(): sftp packet id mismatch" << endl;
        return -1;
        // XXX How do we do a fatal error?
    }

    if( type != SSH2_FXP_STATUS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSetStat(): unexpected message type of " << type << endl;
        return -1;
    }

    Q_UINT32 code;
    r >> code;
    if( code != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSetStat(): set stat failed with err code " << code << endl;
    }

    return code;
}
/** Sends a sftp command to remove a file or directory. */
int kio_sftpProtocol::sftpRemove(const KURL& url, bool isfile){
    QString path = url.path();
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + path.length());
    s << (Q_UINT8)(isfile ? SSH2_FXP_REMOVE : SSH2_FXP_RMDIR);
    s << (Q_UINT32)id;
    s.writeBytes(path.latin1(), path.length());

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::del(): sftp packet id mismatch" << endl;
        return -1;
    }

    if( type != SSH2_FXP_STATUS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::del(): unexpected message type of " << type << endl;
        return -1;
    }

    Q_UINT32 code;
    r >> code;
    if( code != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::del(): del failed with err code " << code << endl;
    }

    return code;
}
/** Send a sftp command to rename a file or directoy. */
int kio_sftpProtocol::sftpRename(const KURL& src, const KURL& dest){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRename(" << src.prettyURL() << ", " << dest.prettyURL() << ")" << endl;

    QString srcPath = src.path();
    QString destPath = dest.path();
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ +
                    4 /*str length*/ + srcPath.length() +
                    4 /*str length*/ + destPath.length());
    s << (Q_UINT8)SSH2_FXP_RENAME;
    s << (Q_UINT32)id;
    s.writeBytes(srcPath.latin1(), srcPath.length());
    s.writeBytes(destPath.latin1(), destPath.length());

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRename(): sftp packet id mismatch" << endl;
        return -1;
    }

    if( type != SSH2_FXP_STATUS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRename(): unexpected message type of " << type << endl;
        return -1;
    }

    int code;
    r >> code;
    if( code != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRename(): rename failed with err code " << code << endl;
    }

    return code;
}
/** Get directory listings. */
int kio_sftpProtocol::sftpReadDir(const QByteArray& handle, const KURL& url){
    // url is needed so we can lookup the link destination
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadDir()" << endl;

    KURL myurl = url;
    sftpFileAttr attr; attr.setDirAttrsFlag(true);
    QByteArray p;
    Q_UINT32 id, expectedId, count;
    Q_UINT8 type;

    QDataStream s(p, IO_WriteOnly);
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + handle.size());
    s << (Q_UINT8)SSH2_FXP_READDIR;
    s << (Q_UINT32)id;
    s << handle;

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);

    r >> type >> id;

    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadDir(): sftp packet id mismatch" << endl;
        return -1;
    }

    int code;
    if( type == SSH2_FXP_STATUS ) {
        r >> code;
        return code;
    }

    if( type != SSH2_FXP_NAME ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocl::sftpReadDir(): Unexpected message" << endl;
        return -1;
    }

    r >> count;
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadDir(): got " << count << " entries" << endl;

    while(count--) {
        r >> attr;

        if( S_ISLNK(attr.permissions()) ) {
            myurl = url;
            myurl.addPath(attr.filename());
            QString target;
            if( (code = sftpReadLink(myurl, target)) == SSH2_FX_OK ) {
                kdDebug(KIO_SFTP_DB) << "got link dest " << target << endl;
                attr.setLinkDestination(target);
            }
        }

        listEntry(attr.entry(), false);
    }

    listEntry(attr.entry(), true);
    return SSH2_FX_OK;
}

kdbgstream& operator<< (kdbgstream& s, QByteArray& a) {
    int i, l = a.size();
    l = 31 < l ? 31 : l; // print no more than first 24 bytes
    QString str;
    for(i = 0; i < l-1; i++)
        s << str.sprintf("%02X ",a[i]);
    s << str.sprintf("%02X",a[i]);
    return s;
}

void mymemcpy(const char* b, QByteArray& a, unsigned int offset, unsigned int len) {
    for(unsigned int i = 0; i < len; i++) {
        a[offset+i] = b[i];
    }
}
/** Retrieves the destination of a link. Not deined in the sftp internet draft.
      uint32 id
      string path
    Returns a SSH_FXP_NAME packet on success with the filename set to the link destination.
    Returns SSH_FXP_STATUS on failure.
    */
int kio_sftpProtocol::sftpReadLink(const KURL& url, QString& target){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadLink(" << url.prettyURL() << ")" << endl;
    QString path = url.path();
    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + path.length());
    s << (Q_UINT8)SSH2_FXP_READLINK;
    s << id;
    s.writeBytes(path.latin1(), path.length());

    putPacket(p);
    getPacket(p);

    Q_UINT8 type;
    QDataStream r(p, IO_ReadOnly);

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadLink: sftp packet id mismatch" << endl;
        return -1;
    }

    if( type == SSH2_FXP_STATUS ) {
        Q_UINT32 code;
        r >> code;
        kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadLink(): read link failed with code " << code << endl;
        return code;
    }

    if( type != SSH2_FXP_NAME ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadLink(): unexpected packet type of " << type << endl;
        return -1;
    }

    Q_UINT32 count;
    r >> count;
    if( count != 1 ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpReadLink(): Bad number of file attributes for realpath command" << endl;
        return -1;
    }

    QByteArray x;
    r >> x;
    QString link(x);
    target = link;
    return SSH2_FX_OK;
}

/** Creates a symlink named dest to target. Not defined in the sftp internet draft.
      uint32 id
      string target
      string destination
    Returns a SSH_FXP_STATUS.
*/
int kio_sftpProtocol::sftpSymLink(const QString& target, const KURL& dest){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSymLink(" << target << ", " << dest.prettyURL() << ")" << endl;
    QString destPath = dest.path();
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ +
                    4 /*str length*/ + target.length() +
                    4 /*str length*/ + destPath.length());
    s << (Q_UINT8)SSH2_FXP_SYMLINK;
    s << (Q_UINT32)id;
    s.writeBytes(target.latin1(), target.length());
    s.writeBytes(destPath.latin1(), destPath.length());

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSymLink(): sftp packet id mismatch" << endl;
        return -1;
    }

    if( type != SSH2_FXP_STATUS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSymLink(): unexpected message type of " << type << endl;
        return -1;
    }

    Q_UINT32 code;
    r >> code;
    if( code != SSH2_FX_OK ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpSymLink(): rename failed with err code " << code << endl;
    }

    return code;
}
/** Stats a file. */
int kio_sftpProtocol::sftpStat(const KURL& url, sftpFileAttr& attr){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpStat()" << endl;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);
    QString path = url.path();

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ + 4 /*str length*/ + path.length());
    s << (Q_UINT8)SSH2_FXP_STAT;
    s << (Q_UINT32)id;
    s.writeBytes(path.latin1(), path.length());

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpStat(): sftp packet id mismatch" << endl;
        return -1;
    }

    if( type == SSH2_FXP_STATUS ) {
        Q_UINT32 errCode;
        r >> errCode;
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpStat(): stat failed with code " << errCode << endl;
        return errCode;
    }

    if( type != SSH2_FXP_ATTRS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpStat(): unexpected message type of " << type << endl;
        return -1;
    }

    r >> attr;
    attr.setFilename(url.filename());
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::stat():: " << attr << endl;
    return SSH2_FX_OK;
}
/** No descriptions */
int kio_sftpProtocol::sftpOpen(const KURL& url, const Q_UINT32 pflags, const sftpFileAttr& attr, QByteArray& handle){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpen(" << url.prettyURL() << ", handle)" << endl;

    QByteArray p;
    QDataStream s(p, IO_WriteOnly);
    QString path = url.path();

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ +
                    4 /*str length*/ + path.length() +
                    4 /*pflags*/ + attr.size());
    s << (Q_UINT8)SSH2_FXP_OPEN;
    s << (Q_UINT32)id;
    s.writeBytes(path.latin1(), path.length());
    s << pflags;
    s << attr;

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpen(): sftp packet id mismatch" << endl;
        return -1;
    }

    if( type == SSH2_FXP_STATUS ) {
        Q_UINT32 errCode;
        r >> errCode;
        return errCode;
    }

    if( type != SSH2_FXP_HANDLE ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpen(): unexpected message type of " << type << endl;
        return -1;
    }

    r >> handle;
    if( handle.size() > 256 ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpen(): handle exceeds max length" << endl;
        return -1;
    }

    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpOpen(): handle (" << handle.size() << "): [" << handle << "]" << endl;
    return SSH2_FX_OK;
}
/** No descriptions */
int kio_sftpProtocol::sftpRead(const QByteArray& handle, Q_UINT32 offset, Q_UINT32 len, QByteArray& data){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRead( offset = " << offset << ", len = " << len << ")" << endl;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ +
                    4 /*str length*/ + handle.size() +
                    8 /*offset*/ + 4 /*length*/);
    s << (Q_UINT8)SSH2_FXP_READ;
    s << (Q_UINT32)id;
    s << handle;
    s << (Q_UINT32)0 << offset; // we don't have a convienient 64 bit int so set upper int to zero
    s << len;

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRead: sftp packet id mismatch" << endl;
        return -1;
    }

    if( type == SSH2_FXP_STATUS ) {
        Q_UINT32 errCode;
        r >> errCode;
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRead: read failed with code " << errCode << endl;
        return errCode;
    }

    if( type != SSH2_FXP_DATA ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpRead: unexpected message type of " << type << endl;
        return -1;
    }

    r >> data;

    return SSH2_FX_OK;
}
/** No descriptions */
int kio_sftpProtocol::sftpWrite(const QByteArray& handle, Q_UINT32 offset, const QByteArray& data){
    kdDebug(KIO_SFTP_DB) << "kio_sftpProtocol::sftpWrite( offset = " << offset << ")" << endl;
    QByteArray p;
    QDataStream s(p, IO_WriteOnly);

    Q_UINT32 id, expectedId;
    id = expectedId = mMsgId++;
    s << (Q_UINT32)(1 /*type*/ + 4 /*id*/ +
                    4 /*str length*/ + handle.size() +
                    8 /*offset*/ + data.size());
    s << (Q_UINT8)SSH2_FXP_WRITE;
    s << (Q_UINT32)id;
    s << handle;
    s << (Q_UINT32)0 << offset; // we don't have a convienient 64 bit int so set upper int to zero
    s << data;

    putPacket(p);
    getPacket(p);

    QDataStream r(p, IO_ReadOnly);
    Q_UINT8 type;

    r >> type >> id;
    if( id != expectedId ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpWrite(): sftp packet id mismatch" << endl;
        return -1;
    }

    if( type != SSH2_FXP_STATUS ) {
        kdError(KIO_SFTP_DB) << "kio_sftpProtocol::sftpWrite(): unexpected message type of " << type << endl;
        return -1;
    }

    Q_UINT32 code;
    r >> code;
    return code;
}