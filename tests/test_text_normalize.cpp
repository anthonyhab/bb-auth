#include "../src/fallback/prompt/TextNormalize.hpp"

#include <QtTest/QtTest>

namespace bb::fallback::prompt {

    class TextNormalizeTest : public QObject {
        Q_OBJECT

      private slots:
        void normalizeDetailText_removesEmptyLines();
        void normalizeDetailText_handlesCRLF();
        void normalizeCompareText_ignoresCase();
        void normalizeCompareText_removesPunctuation();
        void textEquivalent_checksEquality();
        void textEquivalent_checksPrefix();
        void firstMeaningfulLine_returnsFirst();
        void trimToLength_truncates();
        void uniqueJoined_removesDuplicates();
    };

    void TextNormalizeTest::normalizeDetailText_removesEmptyLines() {
        QString input = "Line 1\n\nLine 2\n   \nLine 3";
        QString expected = "Line 1\nLine 2\nLine 3";
        QCOMPARE(normalizeDetailText(input), expected);
    }

    void TextNormalizeTest::normalizeDetailText_handlesCRLF() {
        QString input = "Line 1\r\nLine 2\rLine 3";
        QString expected = "Line 1\nLine 2\nLine 3";
        QCOMPARE(normalizeDetailText(input), expected);
    }

    void TextNormalizeTest::normalizeCompareText_ignoresCase() {
        QString input = "HeLLo WoRLd";
        QString expected = "hello world";
        QCOMPARE(normalizeCompareText(input), expected);
    }

    void TextNormalizeTest::normalizeCompareText_removesPunctuation() {
        QString input = "Hello, \"World\". How`s it going?";
        QCOMPARE(normalizeCompareText(input), QString("hello world how s it going?"));
    }

    void TextNormalizeTest::textEquivalent_checksEquality() {
        QVERIFY(textEquivalent("Foo Bar", "foo bar"));
        QVERIFY(textEquivalent("Foo, Bar", "foo bar"));
    }

    void TextNormalizeTest::textEquivalent_checksPrefix() {
        QVERIFY(textEquivalent("Authentication required", "Authentication required for"));
        QVERIFY(textEquivalent("Authentication required for", "Authentication required"));
        QVERIFY(textEquivalent("Auth", "Authentication"));
    }

    void TextNormalizeTest::firstMeaningfulLine_returnsFirst() {
        QString input = "\n   \nFirst Line\nSecond Line";
        QCOMPARE(firstMeaningfulLine(input), QString("First Line"));
        QCOMPARE(firstMeaningfulLine("Single Line"), QString("Single Line"));
        QCOMPARE(firstMeaningfulLine(""), QString(""));
    }

    void TextNormalizeTest::trimToLength_truncates() {
        QString text = "This is a long text";
        QCOMPARE(trimToLength(text, 10), QString("This is..."));
        QCOMPARE(trimToLength(text, 5), QString("Th..."));
        QCOMPARE(trimToLength(text, 20), text);
    }

    void TextNormalizeTest::uniqueJoined_removesDuplicates() {
        QStringList input = {"A", "B", "a", " b ", "C"};
        QString expected = "A\nB\nC";
        QCOMPARE(uniqueJoined(input), expected);
    }

} // namespace bb::fallback::prompt

int runTextNormalizeTests(int argc, char** argv) {
    bb::fallback::prompt::TextNormalizeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_text_normalize.moc"
