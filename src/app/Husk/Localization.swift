// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import SwiftUI

/// Husk's interface in the player's language. Every line on screen goes through L("..."), which looks the English up in the
/// language's table (Resources/<code>.json) and gives it back translated, or as it is when the table has no entry.
///
/// Interpolations stay interpolations: L("\(name) Added") is looked up as "%@ Added", and the translation puts the value where
/// its own %@ is -- or, for a language that orders things differently, where %1$@, %2$@ say. That is the whole format: every
/// value is a %@, whatever its type, so the key is the English with each \(...) replaced by %@.
enum HuskLanguage: String, CaseIterable, Identifiable {
    case system, english, russian
    var id: String { rawValue }

    static let key = "husk.language"

    /// The player's choice in Settings.
    static var choice: HuskLanguage {
        get { HuskLanguage(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .system }
        set {
            UserDefaults.standard.set(newValue.rawValue, forKey: key)
            Translations.reload()
            Task { @MainActor in LanguageVersion.shared.bump() }
        }
    }

    /// The table in use: the choice, or for "system" the phone's first language Husk has.
    static var effectiveCode: String? {
        switch choice {
        case .english: return nil
        case .russian: return "ru"
        case .system:
            for lang in Locale.preferredLanguages {
                if lang.hasPrefix("ru") { return "ru" }
                if lang.hasPrefix("en") { return nil }
            }
            return nil
        }
    }

    /// The choice's own name, in that language.
    var title: String {
        switch self {
        case .system: return L("Same as iPhone")
        case .english: return "English"
        case .russian: return "Русский"
        }
    }
}

/// Bumped when the language changes, so the interface is drawn again in it.
@MainActor final class LanguageVersion: ObservableObject {
    static let shared = LanguageVersion()
    @Published private(set) var value = 0
    func bump() { value += 1 }
}

enum Translations {
    private static let lock = NSLock()
    private static var table: [String: String] = load()

    private static func load() -> [String: String] {
        guard let code = HuskLanguage.effectiveCode,
              let url = Bundle.main.url(forResource: code, withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let dict = try? JSONDecoder().decode([String: String].self, from: data) else { return [:] }
        return dict
    }

    static func reload() { lock.lock(); table = load(); lock.unlock() }

    static func lookup(_ key: String) -> String? {
        lock.lock(); defer { lock.unlock() }
        return table[key]
    }
}

/// An English line with its interpolated values kept apart, so it can be looked up by its shape.
struct TrString: ExpressibleByStringInterpolation {
    var key = ""
    var values: [String] = []

    init(stringLiteral value: String) { key = value }

    init(stringInterpolation: Interpolation) { key = stringInterpolation.key; values = stringInterpolation.values }

    struct Interpolation: StringInterpolationProtocol {
        var key = ""
        var values: [String] = []
        init(literalCapacity: Int, interpolationCount: Int) { key.reserveCapacity(literalCapacity + 2 * interpolationCount) }
        mutating func appendLiteral(_ literal: String) { key += literal.replacingOccurrences(of: "%", with: "%%") }
        mutating func appendInterpolation<T>(_ value: T) { key += "%@"; values.append(String(describing: value)) }
        mutating func appendInterpolation<T>(_ value: T?) { key += "%@"; values.append(value.map { String(describing: $0) } ?? "") }
    }
}

/// `text` in the interface's language.
func L(_ text: TrString) -> String {
    let format = Translations.lookup(text.key) ?? text.key
    return fill(format, text.values)
}

/// A line made at run time (a status from the runtime, a stored label) in the interface's language, if the table has it.
func Lv(_ text: String) -> String {
    Translations.lookup(text.replacingOccurrences(of: "%", with: "%%")).map { fill($0, []) } ?? text
}

/// The format with its %@ (in order) and %n$@ (by number) replaced by the values, and %% by %.
private func fill(_ format: String, _ values: [String]) -> String {
    var out = ""
    var next = 0
    var i = format.startIndex
    while i < format.endIndex {
        let c = format[i]
        guard c == "%", format.index(after: i) < format.endIndex else { out.append(c); i = format.index(after: i); continue }
        var j = format.index(after: i)
        if format[j] == "%" { out.append("%"); i = format.index(after: j); continue }
        if format[j] == "@" {
            out += next < values.count ? values[next] : ""
            next += 1
            i = format.index(after: j); continue
        }
        // %n$@
        var digits = ""
        while j < format.endIndex, format[j].isNumber { digits.append(format[j]); j = format.index(after: j) }
        if !digits.isEmpty, j < format.endIndex, format[j] == "$", format.index(after: j) < format.endIndex,
           format[format.index(after: j)] == "@", let n = Int(digits) {
            out += n - 1 < values.count && n >= 1 ? values[n - 1] : ""
            i = format.index(j, offsetBy: 2); continue
        }
        out.append(c); i = format.index(after: i)
    }
    return out
}
