/* vi: ts=8 sts=4 sw=4
 *
 * $Id: process.h 112516 2001-08-30 02:36:55Z fisher $
 *
 * This file is part of the KDE project, module kdesu.
 * Copyright (C) 1999,2000 Geert Jansen <jansen@kde.org>
 * 
 * This is free software; you can use this library under the GNU Library 
 * General Public License, version 2. See the file "COPYING.LIB" for the 
 * exact licensing terms.
 */

#ifndef __Process_h_Included__
#define __Process_h_Included__

#include <qcstring.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvaluelist.h>

#define PTYPROC 7120

class PTY;
typedef QValueList<QCString> QCStringList;

/**
 * Synchronous communication with tty programs.
 *
 * PtyProcess provides synchronous communication with tty based programs. 
 * The communications channel used is a pseudo tty (as opposed to a pipe) 
 * This means that programs which require a terminal will work.
 */

class MyPtyProcess
{
public:
    MyPtyProcess();
    virtual ~MyPtyProcess();

    /**
     * Fork off and execute a command. The command's standard in and output 
     * are connected to the pseudo tty. They are accessible with @ref #readLine 
     * and @ref #writeLine.
     * @param command The command to execute.
     * @param args The arguments to the command.
     */
    int exec(QCString command, QCStringList args);

    /**
     * Read a line from the program's standard out. Depending on the @em block 
     * parameter, this call blocks until a single, full line is read. 
     * @param block Block until a full line is read?
     * @return The output string.
     */
    QCString readLine(bool block = true)
        { return readLineFrom(m_Fd, m_ptyBuf, block); }

    QCString readLineFromPty(bool block = true)
        { return readLineFrom(m_Fd, m_ptyBuf, block); }

    QCString readLineFromStdout(bool block = true)
        { return readLineFrom(m_stdinout, m_stdoutBuf, block); }

    QCString readLineFromStderr(bool block = true)
        { return readLineFrom(m_err, m_stderrBuf, block); }

    /**
     * Write a line of text to the program's standard in.
     * @param line The text to write.
     * @param addNewline Adds a '\n' to the line.
     */
    void writeLine(QCString line, bool addNewline=true);

    /**
     * Put back a line of input.
     * @param line The line to put back.
     * @param addNewline Adds a '\n' to the line.
     */

    void unreadLine(QCString line, bool addNewline = true)
        { unreadLineFrom(m_ptyBuf, line, addNewline); }

    void unreadLineFromPty(QCString line, bool addNewline = true)
        { unreadLineFrom(m_ptyBuf, line, addNewline); }

    void unreadLineFromStderr(QCString line, bool addNewline = true)
        { unreadLineFrom(m_stderrBuf, line, addNewline); }

    void unreadLineFromStdout(QCString line, bool addNewline = true)
        { unreadLineFrom(m_stdoutBuf, line, addNewline); }

    /**
     * Set exit string. If a line of program output matches this,
     * @ref #waitForChild() will terminate the program and return.
     */
    void setExitString(QCString exit) { m_Exit = exit; }

    /**
     * Wait for the child to exit. See also @ref #setExitString.
     */
    int waitForChild();

    /**
     * Wait until the pty has cleared the ECHO flag. This is usefull 
     * when programs write a password prompt before they disable ECHO.
     * Disabling it might flush any input that was written.
     */
    int WaitSlave();

    /** Enables/disables local echo on the pseudo tty. */
    int enableLocalEcho(bool enable=true);

    /** Enable/disable terminal output. Relevant only to some subclasses. */
    void setTerminal(bool terminal) { m_bTerminal = terminal; }

    /** Overwritte the password as soon as it is used. Relevant only to
     * some subclasses. */
    void setErase(bool erase) { m_bErase = erase; }

    /** Return the filedescriptor of the process. */
    int fd() {return m_Fd;};

    /** Return the pid of the process. */
    int pid() {return m_Pid;};

    int stdioFd() {return m_stdinout;}

    int stderrFd() {return m_err;}

protected:
    bool m_bErase, m_bTerminal;
    int m_Pid, m_Fd, m_stdinout, m_err;
    QCString m_Command, m_Exit;

private:
    int init();
    int SetupTTY(int fd);

    PTY *m_pPTY;
    QCString m_TTY;
    QCString m_ptyBuf, m_stderrBuf, m_stdoutBuf;

    QCString readLineFrom(int fd, QCString& inbuf, bool block);
    void unreadLineFrom(QCString inbuf, QCString line, bool addnl);
    class PtyProcessPrivate;
    PtyProcessPrivate *d;
};

#endif
