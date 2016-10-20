import serval.log

private func serval_log(level: Int32, format: String, va_list: CVaListPointer) {
    format.withCString { CString in
        vlogMessage(level, __whence, CString, va_list)
    }
}

public func serval_log(level: Int32, text: String) {
    text.withCString { CString in
        withVaList([CString]) { va_list in
            serval_log(level: level, format: "%s", va_list: va_list)
        }
    }
}

public func serval_log_info(_ text: String) {
    serval_log(level: LOG_LEVEL_INFO, text: text)
}

public func serval_log_debug(_ text: String) {
    serval_log(level: LOG_LEVEL_DEBUG, text: text)
}

public let log_level_warn : Int32 = LOG_LEVEL_WARN
