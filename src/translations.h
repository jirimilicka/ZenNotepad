#pragma once
#include <QTranslator>
#include <utility>
#include <vector>

struct TrEntry {
    const char *en, *tr;
};

// Translator backed by small compiled-in tables (no .qm files needed).
class TableTranslator : public QTranslator {
public:
    bool load(const QString &code);
    bool isEmpty() const override { return !table_; }
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;

private:
    const TrEntry *table_ = nullptr;
};

// UI language: "auto" (follow the system), "en", or a code with a built-in table ("cs").
void setUiLanguage(const QString &code);
QString uiLanguage();
// Languages with built-in tables: (code, native name), English first.
std::vector<std::pair<QString, QString>> availableLanguages();
