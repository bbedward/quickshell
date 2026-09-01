#include "toolsupport.hpp"
#include <algorithm>
#include <cerrno>

#include <fcntl.h>
#include <qcontainerfwd.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qqmlengine.h>
#include <qset.h>
#include <qtenvironmentvariables.h>

#include "logcat.hpp"
#include "paths.hpp"
#include "scan.hpp"

namespace qs::core {

namespace {
QS_LOGGING_CATEGORY(logTooling, "quickshell.tooling", QtWarningMsg);
}

bool QmlToolingSupport::updateTooling(const QDir& configRoot) {
	if (!QsPaths::instance()->shellVfsDir()) {
		qCCritical(logTooling) << "Tooling dir could not be created";
		return false;
	}

	if (!QmlToolingSupport::lockTooling()) {
		return false;
	}

	return QmlToolingSupport::updateQmllsConfig(configRoot, false);
}

QString QmlToolingSupport::updateMirror(const QDir& configRoot, const QmlScanner& scanner) {
	auto* vfs = QsPaths::instance()->shellVfsDir();
	if (!vfs) return QString();

	// the engine canonicalizes import paths, so every url derived from this one must match
	auto vfsPath = vfs->canonicalPath();
	if (vfsPath.isEmpty()) return QString();
	if (!QmlToolingSupport::mirrorDir(scanner, configRoot, vfsPath % "/qs")) return QString();

	return vfsPath;
}

bool QmlToolingSupport::mirrorDir(
    const QmlScanner& scanner,
    const QDir& source,
    const QString& target
) {
	const QString prefix = source.path() % '/';

	auto hasIntercepts = std::any_of(
	    scanner.fileIntercepts.keyBegin(),
	    scanner.fileIntercepts.keyEnd(),
	    [&](const QString& path) { return path.startsWith(prefix); }
	);

	if (!hasIntercepts) return QmlToolingSupport::linkMirrorEntry(source.path(), target);

	auto targetInfo = QFileInfo(target);
	if (targetInfo.isSymLink() || (targetInfo.exists() && !targetInfo.isDir())) {
		QmlToolingSupport::removeMirrorEntry(target);
	}

	auto targetDir = QDir(target);

	if (!targetDir.mkpath(".")) {
		qCCritical(logTooling) << "Could not create mirror dir at" << target;
		return false;
	}

	QSet<QString> names;

	for (auto [path, text]: scanner.fileIntercepts.asKeyValueRange()) {
		if (!path.startsWith(prefix)) continue;

		auto name = path.sliced(prefix.length());
		if (name.contains('/')) continue;

		names.insert(name);
		if (!QmlToolingSupport::writeMirrorFile(targetDir.filePath(name), text)) return false;
	}

	auto filters = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;

	for (const auto& name: source.entryList(filters)) {
		if (names.contains(name)) continue;
		names.insert(name);

		auto path = source.filePath(name);
		auto entryPath = targetDir.filePath(name);
		auto ok = QFileInfo(path).isDir() ? QmlToolingSupport::mirrorDir(scanner, QDir(path), entryPath)
		                                  : QmlToolingSupport::linkMirrorEntry(path, entryPath);

		if (!ok) return false;
	}

	for (const auto& name: targetDir.entryList(filters)) {
		if (!names.contains(name)) QmlToolingSupport::removeMirrorEntry(targetDir.filePath(name));
	}

	return true;
}

bool QmlToolingSupport::writeMirrorFile(const QString& path, const QString& text) {
	auto info = QFileInfo(path);
	if (info.isSymLink() || (info.exists() && !info.isFile())) {
		QmlToolingSupport::removeMirrorEntry(path);
	}

	auto file = QFile(path);

	if (!file.open(QFile::ReadWrite)) {
		qCCritical(logTooling) << "Failed to open mirror file" << path;
		return false;
	}

	auto data = text.toUtf8();
	if (file.readAll() == data) return true;

	if (!file.resize(0) || file.write(data) != data.length()) {
		qCCritical(logTooling) << "Failed to write mirror file" << path;
		return false;
	}

	qCDebug(logTooling) << "Wrote mirror file" << path;
	return true;
}

bool QmlToolingSupport::linkMirrorEntry(const QString& path, const QString& linkPath) {
	auto info = QFileInfo(linkPath);
	if (info.isSymLink() && info.symLinkTarget() == path) return true;

	QmlToolingSupport::removeMirrorEntry(linkPath);

	if (!QFile::link(path, linkPath)) {
		qCCritical(logTooling) << "Could not create symlink to" << path << "at" << linkPath;
		return false;
	}

	qCDebug(logTooling) << "Created symlink to" << path << "at" << linkPath;
	return true;
}

void QmlToolingSupport::removeMirrorEntry(const QString& path) {
	auto info = QFileInfo(path);
	if (!info.exists() && !info.isSymLink()) return;

	// isDir follows symlinks, and everything behind one is the real config
	auto ok =
	    info.isDir() && !info.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
	if (!ok) qCWarning(logTooling) << "Failed to remove old file at" << path;
}

bool QmlToolingSupport::lockTooling() {
	if (QmlToolingSupport::toolingLock) return true;

	auto lockPath = QsPaths::instance()->shellVfsDir()->filePath("tooling.lock");
	auto* file = new QFile(lockPath);

	if (!file->open(QFile::WriteOnly)) {
		qCCritical(logTooling) << "Could not open tooling lock for write";
		return false;
	}

	struct flock lock = {
	    .l_type = F_WRLCK,
	    .l_whence = SEEK_SET, // NOLINT (fcntl.h??)
	    .l_start = 0,
	    .l_len = 0,
	    .l_pid = 0,
	};

	if (fcntl(file->handle(), F_SETLK, &lock) == 0) {
		qCInfo(logTooling) << "Acquired tooling support lock";
		QmlToolingSupport::toolingLock = file;
		return true;
	} else if (errno == EACCES || errno == EAGAIN) {
		qCInfo(logTooling) << "Tooling support locked by another instance";
		return false;
	} else {
		qCCritical(logTooling).nospace() << "Could not create tooling lock at " << lockPath
		                                 << " with error code " << errno << ": " << qt_error_string();
		return false;
	}
}

QString QmlToolingSupport::getQmllsConfig() {
	static auto config = []() {
		// We can't replicate the algorithm used to create the import path list as it can have distro
		// specific patches, e.g. nixos.
		auto importPaths = QQmlEngine().importPathList();
		importPaths.removeIf([](const QString& path) { return path.startsWith("qrc:"); });

		auto vfsPath = QsPaths::instance()->shellVfsDir()->path();
		auto importPathsStr = importPaths.join(u':');

		QString qmllsConfig;
		auto print = QDebug(&qmllsConfig).nospace();
		print << "[General]\nno-cmake-calls=true\nbuildDir=" << vfsPath
		      << "\nimportPaths=" << importPathsStr << '\n';

		return qmllsConfig;
	}();

	return config;
}

bool QmlToolingSupport::updateQmllsConfig(const QDir& configRoot, bool create) {
	auto shellConfigPath = configRoot.filePath(".qmlls.ini");
	auto vfsConfigPath = QsPaths::instance()->shellVfsDir()->filePath(".qmlls.ini");

	auto shellFileInfo = QFileInfo(shellConfigPath);
	if (!create && !shellFileInfo.exists() && !shellFileInfo.isSymLink()) {
		if (QmlToolingSupport::toolingEnabled) {
			qInfo() << "QML tooling support disabled";
			QmlToolingSupport::toolingEnabled = false;
		} else {
			qCInfo(logTooling) << "Not enabling QML tooling support, qmlls.ini is missing at path"
			                   << shellConfigPath;
		}

		QFile::remove(vfsConfigPath);
		return false;
	}

	auto vfsFile = QFile(vfsConfigPath);

	if (!vfsFile.open(QFile::ReadWrite | QFile::Text)) {
		qCCritical(logTooling) << "Failed to create qmlls config in vfs";
		return false;
	}

	auto config = QmlToolingSupport::getQmllsConfig();

	if (vfsFile.readAll() != config) {
		if (!vfsFile.resize(0) || !vfsFile.write(config.toUtf8())) {
			qCCritical(logTooling) << "Failed to write qmlls config in vfs";
			return false;
		}

		qCDebug(logTooling) << "Wrote qmlls config in vfs";
	}

	if (!shellFileInfo.isSymLink() || shellFileInfo.symLinkTarget() != vfsConfigPath) {
		QFile::remove(shellConfigPath);

		if (!QFile::link(vfsConfigPath, shellConfigPath)) {
			qCCritical(logTooling) << "Failed to create qmlls config symlink";
			return false;
		}

		qCDebug(logTooling) << "Created qmlls config symlink";
	}

	if (!QmlToolingSupport::toolingEnabled) {
		qInfo() << "QML tooling support enabled";
		QmlToolingSupport::toolingEnabled = true;
	}

	return true;
}

} // namespace qs::core
