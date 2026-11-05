// Runs lumen-expand-helper in test mode (key events from standard input
// instead of keyboards) and checks what it reports over its socket.

#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <linux/input-event-codes.h>

class ExpandHelperTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QProcess m_helper;
    QLocalSocket m_socket;

    // Presses and releases a key (holding `held` keys around it).
    void type(unsigned code, std::initializer_list<unsigned> held = {})
    {
        QByteArray events;
        for (unsigned h : held)
            events += QByteArray::number(h) + " 1\n";
        events += QByteArray::number(code) + " 1\n" + QByteArray::number(code) + " 0\n";
        for (unsigned h : held)
            events += QByteArray::number(h) + " 0\n";
        m_helper.write(events);
    }
    void typeKeyword() // ";addr"
    {
        for (unsigned code : {KEY_SEMICOLON, KEY_A, KEY_D, KEY_D, KEY_R})
            type(code);
    }
    QByteArray reports(int waitMs = 300)
    {
        QTest::qWait(waitMs);
        return m_socket.readAll();
    }

private Q_SLOTS:
    void initTestCase()
    {
        const QString config = m_dir.filePath(QStringLiteral("helper.conf"));
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("layout=us\n");
        f.close();
        const QString socket = m_dir.filePath(QStringLiteral("socket"));
        m_helper.start(QStringLiteral(HELPER_PATH), {QStringLiteral("--test-events"), QStringLiteral("--config"), config,
                                                    QStringLiteral("--socket"), socket});
        QVERIFY(m_helper.waitForStarted());
        QTRY_VERIFY(QFile::exists(socket));
        QCOMPARE(QFile::permissions(socket) & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions()); // owner only
        m_socket.connectToServer(socket);
        QVERIFY(m_socket.waitForConnected(2000));
        m_socket.write("keywords\t;addr\t;sig\n");
        QVERIFY(m_socket.waitForBytesWritten(1000));
        QTest::qWait(100);
    }

    void cleanupTestCase()
    {
        m_helper.closeWriteChannel(); // ends test mode
        m_helper.waitForFinished(2000);
    }

    void reportsKeyword()
    {
        typeKeyword();
        QCOMPARE(reports(), QByteArray("match\t;addr\n"));
    }

    void plainWordsAreIgnored()
    {
        for (unsigned code : {KEY_A, KEY_D, KEY_D, KEY_R, KEY_SPACE})
            type(code);
        QCOMPARE(reports(), QByteArray());
    }

    void backspaceCorrections()
    {
        for (unsigned code : {KEY_SEMICOLON, KEY_A, KEY_D, KEY_X, KEY_BACKSPACE, KEY_D, KEY_R})
            type(code);
        QCOMPARE(reports(), QByteArray("match\t;addr\n"));
    }

    void shortcutsBreakTheWord()
    {
        for (unsigned code : {KEY_SEMICOLON, KEY_A, KEY_D})
            type(code);
        type(KEY_D, {KEY_LEFTCTRL}); // Ctrl+D is a shortcut, not typing
        type(KEY_R);
        QCOMPARE(reports(), QByteArray());
    }

    void layoutIsRespected()
    {
        type(KEY_SEMICOLON, {KEY_LEFTSHIFT}); // ":" on a US layout, not ";"
        for (unsigned code : {KEY_S, KEY_I, KEY_G})
            type(code);
        QCOMPARE(reports(), QByteArray());
        type(KEY_SEMICOLON);
        for (unsigned code : {KEY_S, KEY_I, KEY_G})
            type(code);
        QCOMPARE(reports(), QByteArray("match\t;sig\n"));
    }

    void cursorMovesBreakTheWord()
    {
        for (unsigned code : {KEY_SEMICOLON, KEY_A, KEY_D})
            type(code);
        type(KEY_LEFT);
        for (unsigned code : {KEY_D, KEY_R})
            type(code);
        QCOMPARE(reports(), QByteArray());
    }
};

QTEST_GUILESS_MAIN(ExpandHelperTest)
#include "expandhelpertest.moc"
