// Built-in translations. The UI is written in English; add a language by adding a table.
#include "translations.h"

#include <QCoreApplication>
#include <QLocale>
#include <QString>
#include <cstring>

namespace {
const TrEntry kCzech[] = {
    {"Undo", "Zpět"},
    {"Cut", "Vyjmout"},
    {"Copy", "Kopírovat"},
    {"Paste", "Vložit"},
    {"Delete", "Odstranit"},
    {"Select All", "Vybrat vše"},
    {"Cannot open “%1”:\n%2", "Soubor „%1“ nelze otevřít:\n%2"},
    {"Untitled", "Bez názvu"},
    {"Cannot save “%1”:\n%2", "Soubor „%1“ nelze uložit:\n%2"},
    {"Save changes to “%1”?", "Uložit změny v souboru „%1“?"},
    {"Save As", "Uložit jako"},
    {"Save", "Uložit"},
    {"Don't Save", "Neukládat"},
    {"Cancel", "Zrušit"},
    {"Find", "Hledat"},
    {"Match case", "Rozlišovat velikost písmen"},
    {"Regular expression (PCRE2; in replacement $1, ${name}, \\n, \\t)", "Regulární výraz (PCRE2; v náhradě $1, ${name}, \\n, \\t)"},
    {"Close (Esc)", "Zavřít (Esc)"},
    {"Replace with", "Nahradit čím"},
    {"Replace all (Ctrl+Alt+Enter)", "Nahradit vše (Ctrl+Alt+Enter)"},
    {"Replace All", "Nahradit vše"},
    {"Replace", "Nahradit"},
    {"Wrapped around to the beginning", "Pokračuje od začátku"},
    {"Not found", "Nenalezeno"},
    {"Replaced: %1", "Nahrazeno: %1"},
    {"Open", "Otevřít"},
    {"Wrapped around to the end", "Pokračuje od konce"},
    {"Automatic (system)", "Automaticky (podle systému)"},
    {"Redo", "Znovu"},
    {"Language", "Jazyk"},
    {"Word Wrap", "Zalamovat řádky"},
    {"Inverted Colors", "Inverzní barvy"},
    {"Text Direction", "Směr textu"},
    {"Automatic", "Automaticky"},
    {"Left to Right", "Zleva doprava"},
    {"Right to Left", "Zprava doleva"},
    {"First match from the beginning (Enter)", "První výskyt od začátku (Enter)"},
    {"Next match (F3)", "Další výskyt (F3)"},
    {"Previous match (Shift+F3)", "Předchozí výskyt (Shift+F3)"},
    {"Last match from the end (Shift+Enter)", "Poslední výskyt od konce (Shift+Enter)"},
    {"Enter: replace the first match from the beginning\nShift+Enter: replace the last match from the end\nCtrl+Alt+Enter: replace all",
     "Enter: nahradit první výskyt od začátku\nShift+Enter: nahradit poslední výskyt od konce\nCtrl+Alt+Enter: nahradit vše"},
    {"Replace the selected match and go to the next one", "Nahradit označený výskyt a přejít na další"},
    {nullptr, nullptr},
};
} // namespace

QString TableTranslator::translate(const char *, const char *src, const char *, int) const
{
    for (const TrEntry *e = table_; e && e->en; ++e)
        if (std::strcmp(e->en, src) == 0)
            return QString::fromUtf8(e->tr);
    return QString();
}

bool TableTranslator::load(const QString &code)
{
    QString c = code;
    if (c == QLatin1String("auto"))
        c = QLocale::system().name().section(QLatin1Char('_'), 0, 0);
    table_ = c == QLatin1String("cs") ? kCzech : nullptr;
    return table_ != nullptr;
}

namespace {
TableTranslator *g_translator = nullptr;
QString g_lang = QStringLiteral("auto");
}

void setUiLanguage(const QString &code)
{
    if (!g_translator)
        g_translator = new TableTranslator;
    g_lang = code;
    QCoreApplication::removeTranslator(g_translator);
    if (g_translator->load(code))
        QCoreApplication::installTranslator(g_translator);
}

std::vector<std::pair<QString, QString>> availableLanguages()
{
    return {{QStringLiteral("en"), QStringLiteral("English")}, {QStringLiteral("cs"), QStringLiteral("Čeština")}};
}

QString uiLanguage()
{
    return g_lang;
}
