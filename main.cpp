/*
 * nib - a minimal browser on a Chromium backend.
 *
 * Chrome-less: no toolbar, no menus, no buttons. One hidden command bar and
 * vim-style navigation. The engine is Chromium via QtWebEngine — Blink, V8,
 * Chromium's GPU compositor and its scroll animator.
 */

#include <cstdio>

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QProgressBar>
#include <QStandardPaths>
#include <QString>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <QWebChannel>
#include <QWebEngineCertificateError>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>

static const char *APP_NAME = "nib";
static const int   SCROLL_STEP = 64;   /* px per h/j/k/l */

/* ------------------------------------------------------------------ helpers */

static QString envOr(const char *key, const QString &fallback)
{
	const QByteArray v = qgetenv(key);
	return v.isEmpty() ? fallback : QString::fromLocal8Bit(v);
}

/* NIB_DEBUG=1 traces focus reports and self-tests the scroll path. */
static bool debugMode()
{
	static const bool on = !qgetenv("NIB_DEBUG").isEmpty();
	return on;
}

/*
 * Turn whatever the user typed into something loadable. Explicit schemes and
 * filesystem paths pass through; a bare "host.tld" or "localhost:8080" gets
 * https://; anything else becomes a search.
 */
static QUrl resolveInput(const QString &raw)
{
	const QString s = raw.trimmed();
	if (s.isEmpty())
		return QUrl();

	if (s.contains("://") || s.startsWith("about:") ||
	    s.startsWith("data:") || s.startsWith("mailto:") ||
	    s.startsWith("chrome://"))
		return QUrl::fromUserInput(s);

	if (s.startsWith('/') || s.startsWith('~') || s.startsWith("./")) {
		QString p = s;
		if (p.startsWith('~'))
			p.replace(0, 1, QDir::homePath());
		return QUrl::fromLocalFile(QFileInfo(p).absoluteFilePath());
	}

	if (!s.contains(' ')) {
		const int dot = s.indexOf('.');
		const bool dotted = dot > 0 && dot < s.size() - 1;
		if (dotted || s.contains(':') || s.startsWith("localhost"))
			return QUrl::fromUserInput(s);
	}

	const QString tmpl = envOr("NIB_SEARCH", "https://duckduckgo.com/?q=%s");
	return QUrl(QString(tmpl).replace("%s",
	    QString::fromUtf8(QUrl::toPercentEncoding(s))));
}

/* ----------------------------------------------------------------- app mode */

/*
 * App mode pins the window to one site: a single tab, and main-frame
 * navigation confined to a set of domains. Anything outside is handed to the
 * system browser instead of being loaded here.
 */
struct AppSpec {
	bool        enabled = false;
	QString     id;        /* slug: Wayland app_id and icon name */
	QString     name;
	QStringList scopes;    /* bare domains; subdomains are included */
	QUrl        home;
};
static AppSpec g_app;

/*
 * Reduce a host to the domain an app should be allowed to roam within.
 * A full public suffix list is overkill here, so this handles the common
 * two-part suffixes and otherwise takes the last two labels. Use --scope to
 * override when the guess is wrong.
 */
static QString registrableDomain(const QString &hostIn)
{
	const QString host = hostIn.toLower();
	if (host.isEmpty() || host == QStringLiteral("localhost"))
		return host;
	/* bare IPv4 / IPv6 literal: no notion of a registrable domain */
	if (QRegularExpression(QStringLiteral("^[0-9.]+$")).match(host).hasMatch() ||
	    host.contains(':'))
		return host;

	static const QStringList twoPart = {
	    "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "co.jp", "or.jp",
	    "ne.jp", "com.au", "net.au", "org.au", "edu.au", "co.nz", "org.nz",
	    "co.za", "com.br", "com.mx", "com.ar", "co.in", "com.cn", "com.tr",
	    "com.sg", "com.hk", "co.kr", "com.tw",
	};
	const QStringList l = host.split('.');
	if (l.size() <= 2)
		return host;
	if (twoPart.contains(l.mid(l.size() - 2).join('.')) && l.size() >= 3)
		return l.mid(l.size() - 3).join('.');
	return l.mid(l.size() - 2).join('.');
}

static bool hostInScope(const QString &hostIn)
{
	const QString host = hostIn.toLower();
	for (const QString &s : g_app.scopes) {
		if (host == s || host.endsWith('.' + s))
			return true;
	}
	return false;
}

/* Hand a URL to the system browser. NIB_EXTERNAL overrides the command. */
static void handOff(const QUrl &url)
{
	const QString cmd = envOr("NIB_EXTERNAL", QStringLiteral("xdg-open"));
	if (!QProcess::startDetached(cmd, {url.toString()}))
		fprintf(stderr, "nib: could not run '%s' for %s\n",
		    qUtf8Printable(cmd), qUtf8Printable(url.toString()));
}

static QString slugify(const QString &in)
{
	QString s = in.toLower();
	s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
	    QStringLiteral("-"));
	s.remove(QRegularExpression(QStringLiteral("^-+")));
	s.remove(QRegularExpression(QStringLiteral("-+$")));
	return s.left(48);
}

/* ------------------------------------------------------------------- bridge */

/*
 * Injected JS reports whether the page's focused element takes typing. Without
 * this, bare-key navigation would eat keystrokes meant for text fields.
 */
class FocusBridge : public QObject {
	Q_OBJECT
public:
	using QObject::QObject;
	bool editable = false;
	bool ready = false;   /* false until the page reports in */

public slots:
	void setEditable(bool e)
	{
		editable = e;
		ready = true;
		if (debugMode()) {
			fprintf(stderr, "nib: focus report editable=%d\n", int(e));
			fflush(stderr);
		}
	}
};

static QString focusScript()
{
	QFile f(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
	if (!f.open(QIODevice::ReadOnly)) {
		/* Without the transport there is no focus reporting, and bare-key
		   navigation stays off rather than eating keystrokes. */
		qWarning("nib: qwebchannel.js missing; vim keys disabled");
		return QString();
	}
	const QString chan = QString::fromUtf8(f.readAll());

	return chan + R"JS(
(function () {
  var bridge = null;
  new QWebChannel(qt.webChannelTransport, function (ch) {
    bridge = ch.objects.nib;
    report();
  });

  function report() {
    if (!bridge) return;
    var e = document.activeElement;
    var tag = e ? e.tagName : '';
    /* A focused frame means focus is really inside it, where we cannot see;
       treat that as "typing" so we never steal the key. */
    var takesKeys = !!e && (e.isContentEditable ||
      tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' ||
      tag === 'IFRAME' || tag === 'FRAME' || tag === 'EMBED' ||
      (e.getAttribute && e.getAttribute('role') === 'textbox'));
    bridge.setEditable(takesKeys);
  }

  var soon = function () { setTimeout(report, 0); };
  document.addEventListener('focusin', soon, true);
  document.addEventListener('focusout', soon, true);
  window.addEventListener('pageshow', soon);

  /* Scrolling: prefer the document, else the largest scrollable box on
     screen, so app-shell layouts still respond to j/k. */
  function scrollable(el) {
    if (!el) return false;
    var s = getComputedStyle(el);
    if (!/(auto|scroll|overlay)/.test(s.overflowY + ' ' + s.overflowX))
      return false;
    return el.scrollHeight > el.clientHeight + 1 ||
           el.scrollWidth > el.clientWidth + 1;
  }

  function pick() {
    var d = document.scrollingElement || document.documentElement;
    if (d && (d.scrollHeight > d.clientHeight + 1 ||
              d.scrollWidth > d.clientWidth + 1))
      return d;
    var best = null, area = 0;
    var all = document.body ? document.body.querySelectorAll('*') : [];
    for (var i = 0; i < all.length; i++) {
      var el = all[i];
      if (!scrollable(el)) continue;
      var r = el.getBoundingClientRect();
      var a = Math.max(0, Math.min(r.bottom, innerHeight) - Math.max(r.top, 0)) *
              Math.max(0, Math.min(r.right, innerWidth) - Math.max(r.left, 0));
      if (a > area) { area = a; best = el; }
    }
    return best || d;
  }

  /* Hold the target steady during a keypress burst: re-picking on every key
     let two candidate scrollers alternate, which read as jitter. */
  var cached = null, cachedAt = -1e9, goal = null, lastAt = -1e9;
  var IDLE = 400;

  window.__nibTarget = function () {
    var now = performance.now();
    if (cached && cached.isConnected && now - cachedAt < IDLE) {
      cachedAt = now;
      return cached;
    }
    cached = pick();
    cachedAt = now;
    return cached;
  };

  /*
   * Scroll to an absolute, clamped position rather than by a delta.
   *
   * scrollBy() during a running smooth animation measures from wherever the
   * animation currently is, so at either end of the document the queued
   * deltas fought the animation and the page oscillated. Tracking our own
   * goal and clamping it to the scrollable range makes every keypress
   * idempotent at the limits: once goal hits 0 or max, it stays there.
   */
  function apply(dx, dy, absY, smooth) {
    var t = window.__nibTarget();
    if (!t) return;
    var maxY = Math.max(0, t.scrollHeight - t.clientHeight);
    var maxX = Math.max(0, t.scrollWidth - t.clientWidth);
    var now = performance.now();

    /* Resync only once the animation has settled, so a burst of keys still
       accumulates, but a wheel/trackpad scroll in between is respected. */
    var settled = now - lastAt > IDLE;
    /* A jump much larger than a viewport means something else moved the page
       (anchor link, page script). Respect it even mid-burst. */
    var teleported = goal && Math.abs(t.scrollTop - goal.y) >
        Math.max(200, t.clientHeight * 0.9);
    if (!goal || goal.el !== t || teleported ||
        (settled && (Math.abs(t.scrollTop - goal.y) > 4 ||
                     Math.abs(t.scrollLeft - goal.x) > 4)))
      goal = { el: t, x: t.scrollLeft, y: t.scrollTop };
    lastAt = now;

    goal.y = absY === null ? goal.y + dy : absY;
    goal.x = goal.x + dx;
    goal.y = Math.max(0, Math.min(maxY, goal.y));
    goal.x = Math.max(0, Math.min(maxX, goal.x));

    t.scrollTo({ left: goal.x, top: goal.y,
                 behavior: smooth ? 'smooth' : 'instant' });
  }

  window.__nibScroll = function (dx, dy, smooth) { apply(dx, dy, null, smooth); };

  window.__nibHalf = function (sign) {
    var t = window.__nibTarget();
    if (t) apply(0, sign * t.clientHeight * 0.5, null, true);
  };

  /* A page keeps a couple of lines of overlap so nothing is skipped. */
  window.__nibPage = function (sign) {
    var t = window.__nibTarget();
    if (t) apply(0, sign * Math.max(1, t.clientHeight - 48), null, true);
  };

  window.__nibEnd = function (bottom) {
    var t = window.__nibTarget();
    if (t) apply(0, 0, bottom ? t.scrollHeight : 0, true);
  };
})();
)JS";
}

/* --------------------------------------------------------------------- page */

class Browser;

class Page : public QWebEnginePage {
	Q_OBJECT
public:
	Page(QWebEngineProfile *profile, Browser *browser);
	FocusBridge *focus() const { return m_focus; }

protected:
	QWebEnginePage *createWindow(WebWindowType type) override;
	bool acceptNavigationRequest(const QUrl &url, NavigationType type,
	    bool isMainFrame) override;

private:
	Browser     *m_browser;
	FocusBridge *m_focus;
};

/* ------------------------------------------------------------------ browser */

class Browser : public QWidget {
	Q_OBJECT
public:
	explicit Browser(QWebEngineProfile *profile);

	QWebEngineView *newTab(const QUrl &url, bool focusIt);
	QWebEngineView *adopt(Page *page, bool focusIt);
	void            setStatus(const QString &msg);

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override;

private:
	enum BarMode { BarHidden, BarOpen, BarFind };

	QWebEngineView *currentView() const;
	Page           *currentPage() const;
	void            showBar(BarMode mode, const QString &text);
	void            hideBar();
	void            barActivated();
	void            closeTab(int index);
	void            syncTabs();
	void            refreshChrome();
	void            runJs(const QString &js);
	bool            vimReady() const;
	bool            handleVim(QKeyEvent *ke);
	bool            handleCtrl(QKeyEvent *ke);
	void            zoomBy(double delta, bool reset);
	void            findText(const QString &text, bool backward);
	void            wire(QWebEngineView *view);
	void            selfTest(QWebEngineView *view);
	void            keyTest(QWebEngineView *view);
	void            boundsTest(QWebEngineView *view);
	void            ctrlTest(QWebEngineView *view);
	void            appTest(QWebEngineView *view);
	void            sendKey(int key, Qt::KeyboardModifiers mods);
	bool            m_tested = false;   /* debug tests run once per window */
	bool            m_iconSaved = false;

	QWebEngineProfile *m_profile;
	QTabWidget        *m_tabs;
	QProgressBar      *m_prog;
	QLineEdit         *m_bar;
	QLabel            *m_status;
	BarMode            m_mode = BarHidden;
	bool               m_pendingG = false;
	QString            m_lastFind;
};

/* ---------------------------------------------------------------- page impl */

Page::Page(QWebEngineProfile *profile, Browser *browser)
    : QWebEnginePage(profile, nullptr), m_browser(browser)
{
	m_focus = new FocusBridge(this);
	auto *channel = new QWebChannel(this);
	channel->registerObject(QStringLiteral("nib"), m_focus);
	setWebChannel(channel, QWebEngineScript::ApplicationWorld);

	/* A fresh document has nothing focused until the page says otherwise. */
	connect(this, &QWebEnginePage::loadStarted, this, [this] {
		m_focus->editable = false;
		m_focus->ready = false;
	});

	connect(this, &QWebEnginePage::certificateError, this,
	    [this](QWebEngineCertificateError e) {
		e.rejectCertificate();
		m_browser->setStatus(QStringLiteral("TLS error: %1 — not loaded")
		    .arg(e.description()));
	});

	connect(this, &QWebEnginePage::permissionRequested, this,
	    [this](QWebEnginePermission p) {
		p.deny();
		m_browser->setStatus(QStringLiteral("denied a permission request"));
	});

	connect(this, &QWebEnginePage::linkHovered, this,
	    [this](const QString &url) { m_browser->setStatus(url); });
}

QWebEnginePage *Page::createWindow(WebWindowType type)
{
	Q_UNUSED(type);
	/*
	 * In app mode there is exactly one tab. Returning this page makes a
	 * target=_blank link load in place; scope is still enforced below, so an
	 * off-site link is handed off rather than opened here.
	 */
	if (g_app.enabled)
		return this;

	auto *page = new Page(profile(), m_browser);
	m_browser->adopt(page, true);
	return page;
}

bool Page::acceptNavigationRequest(const QUrl &url, NavigationType type,
    bool isMainFrame)
{
	Q_UNUSED(type);
	if (!g_app.enabled)
		return true;

	/* Only the main frame is confined; iframes are part of the page. */
	if (!isMainFrame)
		return true;

	const QString scheme = url.scheme();
	if (scheme == QStringLiteral("about") || scheme == QStringLiteral("data") ||
	    scheme == QStringLiteral("blob"))
		return true;

	if ((scheme == QStringLiteral("http") || scheme == QStringLiteral("https")) &&
	    hostInScope(url.host()))
		return true;

	handOff(url);
	if (m_browser)
		m_browser->setStatus(QStringLiteral("opened externally: %1")
		    .arg(url.toString()));
	return false;
}

/* ------------------------------------------------------------- browser impl */

Browser::Browser(QWebEngineProfile *profile) : m_profile(profile)
{
	setWindowTitle(APP_NAME);
	resize(1280, 800);

	auto *box = new QVBoxLayout(this);
	box->setContentsMargins(0, 0, 0, 0);
	box->setSpacing(0);

	m_tabs = new QTabWidget(this);
	m_tabs->setDocumentMode(true);
	m_tabs->tabBar()->setVisible(false);
	m_tabs->tabBar()->setExpanding(false);
	m_tabs->tabBar()->setDrawBase(false);

	m_prog = new QProgressBar(this);
	m_prog->setRange(0, 100);
	m_prog->setTextVisible(false);
	m_prog->setFixedHeight(2);
	m_prog->hide();

	m_bar = new QLineEdit(this);
	m_bar->setFrame(false);
	m_bar->hide();

	m_status = new QLabel(this);
	m_status->setTextFormat(Qt::PlainText);
	m_status->hide();

	m_bar->setStyleSheet("QLineEdit { font-family: monospace; padding: 2px 6px; }");
	m_status->setStyleSheet("QLabel { font-family: monospace; font-size: 11px;"
	                        " padding: 1px 6px; color: palette(mid); }");
	m_prog->setStyleSheet("QProgressBar { border: none; background: transparent; }"
	                      "QProgressBar::chunk { background: palette(highlight); }");
	m_tabs->tabBar()->setStyleSheet("QTabBar::tab { padding: 1px 8px; font-size: 11px; }");

	box->addWidget(m_tabs, 1);
	box->addWidget(m_prog);
	box->addWidget(m_bar);
	box->addWidget(m_status);

	connect(m_bar, &QLineEdit::returnPressed, this, &Browser::barActivated);
	connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
		refreshChrome();
		setStatus(QString());
	});

	qApp->installEventFilter(this);
}

QWebEngineView *Browser::currentView() const
{
	return qobject_cast<QWebEngineView *>(m_tabs->currentWidget());
}

Page *Browser::currentPage() const
{
	QWebEngineView *v = currentView();
	return v ? qobject_cast<Page *>(v->page()) : nullptr;
}

void Browser::setStatus(const QString &msg)
{
	m_status->setText(msg);
	m_status->setVisible(!msg.isEmpty());
}

void Browser::runJs(const QString &js)
{
	if (Page *p = currentPage())
		p->runJavaScript(js, QWebEngineScript::ApplicationWorld);
}

bool Browser::vimReady() const
{
	Page *p = currentPage();
	return p && p->focus()->ready && !p->focus()->editable;
}

void Browser::syncTabs()
{
	m_tabs->tabBar()->setVisible(m_tabs->count() > 1);
}

void Browser::refreshChrome()
{
	QWebEngineView *v = currentView();
	if (!v) {
		setWindowTitle(APP_NAME);
		return;
	}
	QString title = v->title();
	if (title.isEmpty())
		title = v->url().toString();
	setWindowTitle(QStringLiteral("%1 — %2")
	    .arg(title.isEmpty() ? QStringLiteral("blank") : title, APP_NAME));
}

void Browser::wire(QWebEngineView *view)
{
	connect(view, &QWebEngineView::titleChanged, this,
	    [this, view](const QString &t) {
		int i = m_tabs->indexOf(view);
		if (i >= 0)
			m_tabs->setTabText(i, t.isEmpty() ? QStringLiteral("blank")
			                                  : t.left(24));
		if (view == currentView())
			refreshChrome();
	});
	connect(view, &QWebEngineView::urlChanged, this, [this, view](const QUrl &) {
		if (view == currentView())
			refreshChrome();
	});
	/*
	 * In app mode, stash the site's favicon under a stable icon name that the
	 * generated .desktop file already points at, so the launcher picks it up
	 * from the second run onward without rewriting the desktop entry.
	 */
	if (g_app.enabled) {
		connect(view, &QWebEngineView::iconChanged, this,
		    [this](const QIcon &icon) {
			if (m_iconSaved || icon.isNull() || g_app.id.isEmpty())
				return;
			const QString dir = QDir(QStandardPaths::writableLocation(
			    QStandardPaths::GenericDataLocation))
			    .filePath(QStringLiteral("icons/hicolor/256x256/apps"));
			QDir().mkpath(dir);
			const QPixmap pm = icon.pixmap(256, 256);
			if (!pm.isNull() && pm.save(QDir(dir).filePath(
			    QStringLiteral("nib-%1.png").arg(g_app.id))))
				m_iconSaved = true;
		});
	}

	connect(view, &QWebEngineView::loadProgress, this, [this, view](int p) {
		if (view != currentView())
			return;
		m_prog->setValue(p);
		m_prog->setVisible(p > 0 && p < 100);
	});
	connect(view, &QWebEngineView::loadFinished, this,
	    [this, view](bool ok) {
		if (view != currentView())
			return;
		m_prog->hide();
		if (!ok)
			setStatus(QStringLiteral("load failed: %1")
			    .arg(view->url().toString()));
		if (ok && debugMode())
			selfTest(view);
	});
}

/*
 * Exercise the injected helpers the way a keypress would, and report what the
 * page's focused element is, so the bare-key gate can be checked without
 * synthesising input.
 */
void Browser::selfTest(QWebEngineView *view)
{
	auto *page = qobject_cast<Page *>(view->page());
	if (!page)
		return;
	static const char *js = R"JS(
	(function () {
	  var t = window.__nibTarget ? window.__nibTarget() : null;
	  if (!t) return 'FAIL: helpers not injected';
	  var before = t.scrollTop;
	  window.__nibScroll(0, 64, false);
	  var a = document.activeElement;
	  return 'target=' + t.tagName +
	         ' scroll ' + before + '->' + t.scrollTop +
	         ' activeElement=' + (a ? a.tagName : 'none');
	})();
	)JS";
	page->runJavaScript(QString::fromUtf8(js),
	    QWebEngineScript::ApplicationWorld, [page](const QVariant &r) {
		fprintf(stderr, "nib: selftest %s | gate=%s\n",
		    qUtf8Printable(r.toString()),
		    page->focus()->ready
		        ? (page->focus()->editable ? "keys->page" : "keys->nib")
		        : "not-ready");
		fflush(stderr);
	});

	if (m_tested)
		return;
	const QByteArray mode = qgetenv("NIB_DEBUG");
	if (mode == "keys" || mode == "bounds" || mode == "ctrl" || mode == "app") {
		m_tested = true;
		if (mode == "keys")
			keyTest(view);
		else if (mode == "bounds")
			boundsTest(view);
		else if (mode == "ctrl")
			ctrlTest(view);
		else
			appTest(view);
	}
}

void Browser::sendKey(int key, Qt::KeyboardModifiers mods)
{
	QWidget *target = QApplication::focusWidget();
	QKeyEvent press(QEvent::KeyPress, key, mods);
	QApplication::sendEvent(target ? target : this, &press);
}

/*
 * Exercise C-j page-down through the real key path.
 *
 * The clipboard bindings (C-y, C-p) are deliberately NOT tested here: on
 * Wayland only the focused client may touch the selection, so the result
 * depends on window focus, and taking ownership then exiting destroys
 * whatever the user had copied. Verified once by hand instead.
 */
void Browser::ctrlTest(QWebEngineView *view)
{
	auto *page = qobject_cast<Page *>(view->page());
	if (!page)
		return;

	fprintf(stderr, "nib: ctrl platform=%s\n",
	    qUtf8Printable(QGuiApplication::platformName()));
	fflush(stderr);

	/* Wait past the scroll goal's idle window so it resyncs to the real
	   position, then measure the delta the keypress actually produces. */
	QTimer::singleShot(700, this, [this, page] {
		page->runJavaScript(QStringLiteral(
		    "(function(){var t=window.__nibTarget();return t.scrollTop"
		    " + ' ' + Math.max(1, t.clientHeight - 48);})()"),
		    QWebEngineScript::ApplicationWorld,
		    [this, page](const QVariant &base) {
			const QStringList b = base.toString().split(' ');
			auto *before = new double(b.value(0).toDouble());
			auto *expect = new double(b.value(1).toDouble());
			sendKey(Qt::Key_J, Qt::ControlModifier);

			QTimer::singleShot(900, this, [page, before, expect] {
				page->runJavaScript(
				    QStringLiteral("window.__nibTarget().scrollTop"),
				    QWebEngineScript::ApplicationWorld,
				    [before, expect](const QVariant &v) {
					const double moved = v.toDouble() - *before;
					fprintf(stderr,
					    "nib: ctrl C-j page-down moved %.1f of %.1f %s\n",
					    moved, *expect,
					    qAbs(moved - *expect) <= 2.0 ? "OK" : "FAIL");
					fflush(stderr);
					delete before;
					delete expect;
				});
			});
		});
	});
}

/*
 * Drive the real key path: a synthetic KeyPress to the focused widget goes
 * through QApplication::notify, which is exactly where our event filter sits.
 */
void Browser::keyTest(QWebEngineView *view)
{
	auto *page = qobject_cast<Page *>(view->page());
	if (!page)
		return;

	QTimer::singleShot(600, this, [this, page] {
		page->runJavaScript(QStringLiteral("window.__nibTarget()"
		    ".scrollTo({top:0,behavior:'instant'}); 0"),
		    QWebEngineScript::ApplicationWorld);

		QWidget *target = QApplication::focusWidget();
		fprintf(stderr, "nib: keytest focusWidget=%s\n",
		    target ? target->metaObject()->className() : "none");

		for (int i = 0; i < 3; i++)
			sendKey(Qt::Key_J, Qt::NoModifier);

		QTimer::singleShot(700, this, [page] {
			page->runJavaScript(QStringLiteral("window.__nibTarget().scrollTop"),
			    QWebEngineScript::ApplicationWorld,
			    [](const QVariant &v) {
				fprintf(stderr, "nib: keytest scrollTop after 3x j = %s"
				    " (expect %d)\n", qUtf8Printable(v.toString()),
				    3 * SCROLL_STEP);
				fflush(stderr);
			});
		});
	});
}

/*
 * Hammer both scroll limits: repeated keys past the end must clamp and stay
 * put, and a burst of smooth page-downs at the bottom must settle exactly at
 * the maximum rather than bouncing.
 */
void Browser::boundsTest(QWebEngineView *view)
{
	auto *page = qobject_cast<Page *>(view->page());
	if (!page)
		return;
	static const char *js = R"JS(
	(function () {
	  var t = window.__nibTarget();
	  var max = t.scrollHeight - t.clientHeight;
	  var out = [];

	  t.scrollTo({ top: max, behavior: 'instant' });
	  for (var i = 0; i < 6; i++) window.__nibScroll(0, 64, false);
	  out.push('past-bottom ' + t.scrollTop + '/' + max +
	           (Math.abs(t.scrollTop - max) <= 1 ? ' OK' : ' FAIL'));

	  t.scrollTo({ top: 0, behavior: 'instant' });
	  for (var j = 0; j < 6; j++) window.__nibScroll(0, -64, false);
	  out.push('past-top ' + t.scrollTop +
	           (Math.abs(t.scrollTop) <= 1 ? ' OK' : ' FAIL'));

	  /* now queue smooth page-downs while already pinned at the bottom */
	  t.scrollTo({ top: max, behavior: 'instant' });
	  for (var k = 0; k < 6; k++) window.__nibPage(1);
	  return out.join(' | ');
	})();
	)JS";

	page->runJavaScript(QString::fromUtf8(js),
	    QWebEngineScript::ApplicationWorld, [this, page](const QVariant &r) {
		fprintf(stderr, "nib: bounds %s\n", qUtf8Printable(r.toString()));
		fflush(stderr);

		/* sample twice, well after any animation would have finished */
		for (int delay : {1200, 2200}) {
			QTimer::singleShot(delay, this, [page, delay] {
				page->runJavaScript(QStringLiteral(
				    "(function(){var t=window.__nibTarget();return t.scrollTop"
				    " + '/' + (t.scrollHeight - t.clientHeight);})()"),
				    QWebEngineScript::ApplicationWorld,
				    [delay](const QVariant &v) {
					const QString s = v.toString();
					const QStringList p = s.split('/');
					const bool pinned = p.size() == 2 &&
					    qAbs(p[0].toDouble() - p[1].toDouble()) <= 1.0;
					fprintf(stderr, "nib: bounds settle@%dms %s %s\n", delay,
					    qUtf8Printable(s), pinned ? "OK" : "FAIL");
					fflush(stderr);
				});
			});
		}
	});
}

/*
 * App-mode confinement: C-t must not open a tab, and a script-initiated
 * navigation off-scope must be refused and handed off instead.
 */
void Browser::appTest(QWebEngineView *view)
{
	auto *page = qobject_cast<Page *>(view->page());
	if (!page)
		return;
	fprintf(stderr, "nib: app id=%s scope=%s tabs=%d\n",
	    qUtf8Printable(g_app.id), qUtf8Printable(g_app.scopes.join(",")),
	    m_tabs->count());

	QTimer::singleShot(400, this, [this, view] {
		sendKey(Qt::Key_T, Qt::ControlModifier);
		QTimer::singleShot(300, this, [this, view] {
			fprintf(stderr, "nib: app after C-t tabs=%d %s\n", m_tabs->count(),
			    m_tabs->count() == 1 ? "OK" : "FAIL");

			const QString before = view->url().toString();
			auto *was = new QString(before);
			view->page()->runJavaScript(QStringLiteral(
			    "location.href='https://example.com/off-scope'"),
			    QWebEngineScript::ApplicationWorld);

			QTimer::singleShot(1500, this, [view, was] {
				const QString now = view->url().toString();
				fprintf(stderr, "nib: app off-scope nav url=%s %s\n",
				    qUtf8Printable(now), now == *was ? "OK (refused)" : "FAIL");
				fflush(stderr);
				delete was;
			});
		});
	});
}

/* ------------------------------------------------------------------- tabs */

QWebEngineView *Browser::adopt(Page *page, bool focusIt)
{
	auto *view = new QWebEngineView(this);
	view->setPage(page);
	const int index = m_tabs->addTab(view, QStringLiteral("blank"));
	wire(view);
	syncTabs();
	if (focusIt) {
		m_tabs->setCurrentIndex(index);
		view->setFocus();
	}
	return view;
}

QWebEngineView *Browser::newTab(const QUrl &url, bool focusIt)
{
	auto *page = new Page(m_profile, this);
	QWebEngineView *view = adopt(page, focusIt);
	if (url.isValid())
		view->setUrl(url);
	return view;
}

void Browser::closeTab(int index)
{
	if (index < 0)
		return;
	if (m_tabs->count() <= 1) {
		close();
		return;
	}
	QWidget *w = m_tabs->widget(index);
	m_tabs->removeTab(index);
	w->deleteLater();
	syncTabs();
	if (QWebEngineView *v = currentView())
		v->setFocus();
}

/* --------------------------------------------------------------- command bar */

void Browser::showBar(BarMode mode, const QString &text)
{
	m_mode = mode;
	m_bar->setPlaceholderText(mode == BarFind ? QStringLiteral("find")
	                                          : QStringLiteral("open"));
	m_bar->setText(text);
	m_bar->show();
	m_bar->setFocus();
	m_bar->selectAll();
}

void Browser::hideBar()
{
	m_mode = BarHidden;
	m_bar->hide();
	if (QWebEngineView *v = currentView())
		v->setFocus();
}

void Browser::findText(const QString &text, bool backward)
{
	Page *p = currentPage();
	if (!p)
		return;
	m_lastFind = text;
	QWebEnginePage::FindFlags flags;
	if (backward)
		flags |= QWebEnginePage::FindBackward;
	p->findText(text, flags);
}

void Browser::barActivated()
{
	const QString text = m_bar->text();
	if (m_mode == BarFind) {
		findText(text, false);
		return;   /* leave the bar up so Enter/n steps through matches */
	}
	const QUrl url = resolveInput(text);
	hideBar();
	if (url.isValid()) {
		if (QWebEngineView *v = currentView())
			v->setUrl(url);
	}
}

void Browser::zoomBy(double delta, bool reset)
{
	QWebEngineView *v = currentView();
	if (!v)
		return;
	const double z = reset ? 1.0 : qBound(0.3, v->zoomFactor() + delta, 5.0);
	v->setZoomFactor(z);
	setStatus(QStringLiteral("zoom %1%").arg(qRound(z * 100)));
}

/* ------------------------------------------------------------------ keys */

bool Browser::handleCtrl(QKeyEvent *ke)
{
	const bool shift = ke->modifiers() & Qt::ShiftModifier;
	QWebEngineView *v = currentView();

	switch (ke->key()) {
	case Qt::Key_G:
		showBar(BarOpen, v ? v->url().toString() : QString());
		return true;
	case Qt::Key_F:
		showBar(BarFind, QString());
		return true;
	case Qt::Key_H:
		if (v) v->back();
		return true;
	case Qt::Key_L:
		if (v) v->forward();
		return true;
	case Qt::Key_J:
		runJs(QStringLiteral("__nibPage(1)"));
		return true;
	case Qt::Key_K:
		runJs(QStringLiteral("__nibPage(-1)"));
		return true;
	case Qt::Key_Y: {
		if (!v)
			return true;
		const QString url = v->url().toString();
		QGuiApplication::clipboard()->setText(url);
		setStatus(QStringLiteral("yanked %1").arg(url));
		return true;
	}
	case Qt::Key_P: {
		if (shift) {
			if (Page *p = currentPage()) {
				const QString out = QDir(QStandardPaths::writableLocation(
				    QStandardPaths::DownloadLocation))
				    .filePath(QStringLiteral("nib-print.pdf"));
				p->printToPdf(out);
				setStatus(QStringLiteral("printed to %1").arg(out));
			}
			return true;
		}
		QClipboard *cb = QGuiApplication::clipboard();
		QString text = cb->text();
		if (text.trimmed().isEmpty() && cb->supportsSelection())
			text = cb->text(QClipboard::Selection);
		const QUrl u = resolveInput(text);
		if (text.trimmed().isEmpty() || !u.isValid()) {
			setStatus(QStringLiteral("clipboard has nothing to open"));
			return true;
		}
		if (v) {
			v->setUrl(u);
			setStatus(QStringLiteral("opened %1").arg(u.toString()));
		}
		return true;
	}
	case Qt::Key_R:
		if (v)
			shift ? v->triggerPageAction(QWebEnginePage::ReloadAndBypassCache)
			      : v->reload();
		return true;
	case Qt::Key_T:
		if (g_app.enabled) {
			setStatus(QStringLiteral("app mode: single tab only"));
			return true;
		}
		newTab(QUrl(), true);
		showBar(BarOpen, QString());
		return true;
	case Qt::Key_W:
		if (g_app.enabled) {
			close();
			return true;
		}
		closeTab(m_tabs->currentIndex());
		return true;
	case Qt::Key_Q:
		close();
		return true;
	case Qt::Key_BracketLeft:
		if (v) v->back();
		return true;
	case Qt::Key_BracketRight:
		if (v) v->forward();
		return true;
	case Qt::Key_Tab:
		m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
		return true;
	case Qt::Key_Backtab:
		m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count())
		    % m_tabs->count());
		return true;
	case Qt::Key_Plus:
	case Qt::Key_Equal:
		zoomBy(0.1, false);
		return true;
	case Qt::Key_Minus:
		zoomBy(-0.1, false);
		return true;
	case Qt::Key_0:
		zoomBy(0, true);
		return true;
	default:
		return false;
	}
}

bool Browser::handleVim(QKeyEvent *ke)
{
	QWebEngineView *v = currentView();
	const int key = ke->key();
	const bool shift = ke->modifiers() & Qt::ShiftModifier;

	if (m_pendingG) {
		m_pendingG = false;
		if (key == Qt::Key_G && !shift) {
			runJs(QStringLiteral("__nibEnd(false)"));
			return true;
		}
	}

	switch (key) {
	case Qt::Key_J:
		runJs(QStringLiteral("__nibScroll(0,%1,false)").arg(SCROLL_STEP));
		return true;
	case Qt::Key_K:
		runJs(QStringLiteral("__nibScroll(0,%1,false)").arg(-SCROLL_STEP));
		return true;
	case Qt::Key_H:
		if (shift) {
			if (v) v->back();
		} else {
			runJs(QStringLiteral("__nibScroll(%1,0,false)").arg(-SCROLL_STEP));
		}
		return true;
	case Qt::Key_L:
		if (shift) {
			if (v) v->forward();
		} else {
			runJs(QStringLiteral("__nibScroll(%1,0,false)").arg(SCROLL_STEP));
		}
		return true;
	case Qt::Key_D:
		runJs(QStringLiteral("__nibHalf(1)"));
		return true;
	case Qt::Key_U:
		runJs(QStringLiteral("__nibHalf(-1)"));
		return true;
	case Qt::Key_G:
		if (shift)
			runJs(QStringLiteral("__nibEnd(true)"));
		else
			m_pendingG = true;
		return true;
	case Qt::Key_N:
		if (!m_lastFind.isEmpty())
			findText(m_lastFind, shift);
		return true;
	case Qt::Key_Slash:
		showBar(BarFind, QString());
		return true;
	case Qt::Key_O:
		showBar(BarOpen, v ? v->url().toString() : QString());
		return true;
	default:
		return false;
	}
}

bool Browser::eventFilter(QObject *obj, QEvent *ev)
{
	Q_UNUSED(obj);
	if (ev->type() != QEvent::KeyPress)
		return false;
	if (!isActiveWindow())
		return false;

	auto *ke = static_cast<QKeyEvent *>(ev);
	const bool ctrl = ke->modifiers() & Qt::ControlModifier;
	const bool alt = ke->modifiers() & Qt::AltModifier;

	if (ke->key() == Qt::Key_Escape) {
		m_pendingG = false;
		if (m_mode != BarHidden) {
			if (m_mode == BarFind)
				findText(QString(), false);   /* clear highlights */
			hideBar();
			setStatus(QString());
			return true;
		}
		if (QWebEngineView *v = currentView())
			v->triggerPageAction(QWebEnginePage::Stop);
		setStatus(QString());
		return true;
	}

	/*
	 * While typing in the command bar, the bar owns every key. Escape is
	 * handled above; the rest — including Ctrl combos, several of which are
	 * line-editing conventions — belongs to the QLineEdit.
	 */
	if (m_bar->hasFocus())
		return false;

	if (alt && !ctrl) {
		if (ke->key() == Qt::Key_Left) {
			if (QWebEngineView *v = currentView()) v->back();
			return true;
		}
		if (ke->key() == Qt::Key_Right) {
			if (QWebEngineView *v = currentView()) v->forward();
			return true;
		}
		if (ke->key() >= Qt::Key_1 && ke->key() <= Qt::Key_9) {
			const int want = ke->key() - Qt::Key_1;
			m_tabs->setCurrentIndex(qMin(want, m_tabs->count() - 1));
			return true;
		}
		return false;
	}

	if (ctrl)
		return handleCtrl(ke);

	/* Bare keys: only when the page has told us nothing is taking typing. */
	if (m_mode == BarHidden && vimReady())
		return handleVim(ke);

	return false;
}

/* -------------------------------------------------------------------- setup */

/*
 * The binary ships its own Qt/Chromium runtime beside itself, so point
 * QtWebEngine at it before QApplication starts.
 */
static void bundleInit(const char *argv0)
{
	Q_UNUSED(argv0);
	const QString exe = QFileInfo(QStringLiteral("/proc/self/exe"))
	    .canonicalFilePath();
	const QDir root = QFileInfo(exe).dir();          /* .../bin */
	const QString lib = QDir(root).filePath(QStringLiteral("../lib/nib"));
	if (!QFileInfo::exists(lib))
		return;
	const QDir b(QFileInfo(lib).canonicalFilePath());

	qputenv("QTWEBENGINEPROCESS_PATH",
	    b.filePath("libexec/QtWebEngineProcess").toLocal8Bit());
	qputenv("QTWEBENGINE_RESOURCES_PATH",
	    b.filePath("resources").toLocal8Bit());
	qputenv("QTWEBENGINE_LOCALES_PATH",
	    b.filePath("translations/qtwebengine_locales").toLocal8Bit());
	qputenv("QT_PLUGIN_PATH", b.filePath("plugins").toLocal8Bit());
	qputenv("QML_IMPORT_PATH", b.filePath("qml").toLocal8Bit());
	qputenv("QML2_IMPORT_PATH", b.filePath("qml").toLocal8Bit());

	/* Our own libs come from the rpath; the Chromium helper process is
	   exec'd separately and needs to find them too. */
	const QByteArray prev = qgetenv("LD_LIBRARY_PATH");
	QByteArray ldp = b.filePath(QStringLiteral("lib")).toLocal8Bit();
	if (!prev.isEmpty())
		ldp += ":" + prev;
	qputenv("LD_LIBRARY_PATH", ldp);
}

/* % introduces a field code in Exec, so a literal one must be doubled. */
static QString execArg(const QString &s)
{
	QString e = s;
	e.replace('%', QStringLiteral("%%"));
	if (e.contains(' ') || e.contains('"'))
		e = '"' + QString(e).replace('"', QStringLiteral("\\\"")) + '"';
	return e;
}

/*
 * Write a launcher that starts nib pinned to one site. The icon name is
 * reserved up front; the favicon lands there on first run.
 */
static int installApp(const QUrl &url, const QString &nameIn,
    const QStringList &scopes, const QString &icon, const QString &profile)
{
	if (!url.isValid() || url.host().isEmpty()) {
		fputs("nib: --install-app needs an http(s) URL\n", stderr);
		return 2;
	}
	const QString name = nameIn.isEmpty() ? url.host() : nameIn;
	const QString slug = slugify(name);
	if (slug.isEmpty()) {
		fputs("nib: could not derive a name; pass --name\n", stderr);
		return 2;
	}

	const QString dir = QDir(QStandardPaths::writableLocation(
	    QStandardPaths::GenericDataLocation))
	    .filePath(QStringLiteral("applications"));
	QDir().mkpath(dir);
	const QString path = QDir(dir).filePath(
	    QStringLiteral("nib-%1.desktop").arg(slug));

	QStringList exec;
	exec << execArg(QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath())
	     << execArg(QStringLiteral("--app=") + url.toString());
	if (!scopes.isEmpty())
		exec << execArg(QStringLiteral("--scope=") + scopes.join(','));
	if (!profile.isEmpty())
		exec << execArg(QStringLiteral("--profile=") + profile);

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		fprintf(stderr, "nib: cannot write %s\n", qUtf8Printable(path));
		return 1;
	}
	QTextStream out(&f);
	out << "[Desktop Entry]\n"
	    << "Type=Application\n"
	    << "Name=" << name << "\n"
	    << "Comment=" << url.host() << " (nib app)\n"
	    << "Exec=" << exec.join(' ') << "\n"
	    << "Icon=" << (icon.isEmpty() ? QStringLiteral("nib-") + slug : icon) << "\n"
	    << "Terminal=false\n"
	    << "Categories=Network;\n"
	    << "StartupNotify=true\n"
	    /* Wayland matches app_id; StartupWMClass covers X11 */
	    << "StartupWMClass=nib-" << slug << "\n"
	    << "X-nib-app=" << slug << "\n";
	f.close();

	printf("wrote %s\n  name  %s\n  url   %s\n  scope %s\n  icon  %s\n",
	    qUtf8Printable(path), qUtf8Printable(name),
	    qUtf8Printable(url.toString()), qUtf8Printable(scopes.join(", ")),
	    qUtf8Printable(icon.isEmpty() ? "nib-" + slug + " (favicon on first run)"
	                                  : icon));
	return 0;
}

static const char *usage =
"usage: nib [URL|FILE ...]\n"
"\n"
"A minimal keyboard-driven browser on a Chromium (QtWebEngine) backend.\n"
"Each argument opens in a tab; with none, NIB_HOME is opened. Non-URL text\n"
"is sent to the search engine.\n"
"\n"
"vim keys (only when the page has no text field focused):\n"
"  h j k l     scroll left/down/up/right      d / u     half page down / up\n"
"  gg / G      top / bottom                   H / L     back / forward\n"
"  / , n , N   find, next, previous           o         open URL bar\n"
"\n"
"always:\n"
"  C-g  URL bar      C-f  find        C-j / C-k  page down / up\n"
"  C-h / C-l  back / forward          C-[ / C-]  back / forward  M-Left/Right too\n"
"  C-y  yank URL     C-p  open URL from clipboard\n"
"  C-t  new tab      C-w  close tab   C-Tab      next tab       M-1..9  nth tab\n"
"  C-r  reload       C-S-r  no cache  C-S-p      print to PDF\n"
"  C-+ / C-- / C-0   zoom             C-q        quit\n"
"  Escape            stop loading, or dismiss the bar\n"
"\n"
"n / N still step through matches; the command bar keeps its own Ctrl keys\n"
"for line editing while it has focus.\n"
"\n"
"app mode (PWA-style, pinned to one site):\n"
"  --app=URL            single tab, confined to URL's registrable domain\n"
"  --scope=a.com,b.com  override the allowed domains (subdomains included)\n"
"  --profile=NAME       separate cookie jar for this app\n"
"  --name=NAME          window/launcher name (also the icon and app_id slug)\n"
"  --install-app URL    write ~/.local/share/applications/nib-<slug>.desktop\n"
"  --check-scope U...   print ALLOW/HANDOFF per URL and exit, no window\n"
"\n"
"In app mode C-t is refused, C-w quits, and off-scope main-frame navigation\n"
"is opened in the system browser instead ($NIB_EXTERNAL, default xdg-open).\n"
"\n"
"env:\n"
"  NIB_HOME    start page          (default https://duckduckgo.com)\n"
"  NIB_EXTERNAL  command for off-scope URLs (default xdg-open)\n"
"  NIB_SEARCH  search URL, %s slot (default DuckDuckGo)\n"
"  NIB_UA      override user agent\n"
"\n"
"state: profile under ~/.local/share/nib, cache under ~/.cache/nib\n";

int main(int argc, char **argv)
{
	QString appUrl, appName, appIcon, appProfile, scopeArg;
	bool doInstall = false;
	QStringList checkScope, positional;

	for (int i = 1; i < argc; i++) {
		const QString a = QString::fromLocal8Bit(argv[i]);
		if (a == "-h" || a == "--help") {
			fputs(usage, stdout);
			return 0;
		}
		if (a == "-v" || a == "--version") {
			printf("%s (QtWebEngine %s, Chromium backend)\n",
			    APP_NAME, qVersion());
			return 0;
		}
		/* accept both --opt=value and --opt value */
		auto opt = [&](const char *name, QString &into) {
			const QString flag = QLatin1String("--") + QLatin1String(name);
			if (a == flag && i + 1 < argc) {
				into = QString::fromLocal8Bit(argv[++i]);
				return true;
			}
			if (a.startsWith(flag + '=')) {
				into = a.mid(flag.size() + 1);
				return true;
			}
			return false;
		};

		if (opt("app", appUrl) || opt("scope", scopeArg) ||
		    opt("name", appName) || opt("icon", appIcon) ||
		    opt("profile", appProfile))
			continue;
		if (a == "--install-app")            doInstall = true;
		else if (a == "--check-scope")       checkScope << QStringLiteral("-");
		else if (!checkScope.isEmpty())      checkScope << a;
		else if (!a.startsWith('-'))         positional << a;
	}

	/* --app=URL, or the first positional when installing */
	if (appUrl.isEmpty() && doInstall && !positional.isEmpty())
		appUrl = positional.first();

	if (!appUrl.isEmpty()) {
		g_app.enabled = true;
		g_app.home = resolveInput(appUrl);
		g_app.name = appName.isEmpty() ? g_app.home.host() : appName;
		g_app.id = slugify(g_app.name);
		if (scopeArg.isEmpty()) {
			g_app.scopes << registrableDomain(g_app.home.host());
		} else {
			for (const QString &s : scopeArg.split(',', Qt::SkipEmptyParts))
				g_app.scopes << s.trimmed().toLower();
		}
	}

	if (doInstall)
		return installApp(g_app.home, appName, g_app.scopes, appIcon, appProfile);

	/* Report navigation decisions and exit — no window, no session churn. */
	if (!checkScope.isEmpty()) {
		if (!g_app.enabled) {
			fputs("nib: --check-scope needs --app=URL\n", stderr);
			return 2;
		}
		printf("app   %s\nscope %s\n", qUtf8Printable(g_app.name),
		    qUtf8Printable(g_app.scopes.join(", ")));
		for (const QString &raw : checkScope) {
			if (raw == "-")
				continue;
			const QUrl u = resolveInput(raw);
			const QString scheme = u.scheme();
			const bool special = scheme == "about" || scheme == "data" ||
			                     scheme == "blob";
			const bool ok = special ||
			    ((scheme == "http" || scheme == "https") && hostInScope(u.host()));
			printf("  %-7s %s\n", ok ? "ALLOW" : "HANDOFF",
			    qUtf8Printable(u.toString()));
		}
		return 0;
	}

	bundleInit(argv[0]);
	QApplication app(argc, argv);
	QApplication::setApplicationName(g_app.enabled
	    ? g_app.name : QString::fromLatin1(APP_NAME));
	/* app_id on Wayland comes from the desktop file name: this is what makes
	   the launcher group the window and show the app's own icon */
	QApplication::setDesktopFileName(g_app.enabled
	    ? QStringLiteral("nib-") + g_app.id : QStringLiteral("nib"));

	/* A named --profile gets its own cookie jar, so an app can hold a second
	   login without disturbing the main browser session. */
	const QString leaf = appProfile.isEmpty()
	    ? QString::fromLatin1(APP_NAME)
	    : QStringLiteral("%1/profiles/%2").arg(APP_NAME, slugify(appProfile));
	const QString data = QDir(QStandardPaths::writableLocation(
	    QStandardPaths::GenericDataLocation)).filePath(leaf);
	const QString cache = QDir(QStandardPaths::writableLocation(
	    QStandardPaths::GenericCacheLocation)).filePath(leaf);

	auto *profile = new QWebEngineProfile(appProfile.isEmpty()
	    ? QString::fromLatin1(APP_NAME) : slugify(appProfile));
	profile->setPersistentStoragePath(data);
	profile->setCachePath(cache);
	profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
	profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
	profile->setDownloadPath(QStandardPaths::writableLocation(
	    QStandardPaths::DownloadLocation));
	const QByteArray ua = qgetenv("NIB_UA");
	if (!ua.isEmpty())
		profile->setHttpUserAgent(QString::fromLocal8Bit(ua));

	QWebEngineSettings *s = profile->settings();
	s->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, true);
	s->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
	s->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
	s->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
	s->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, true);

	QWebEngineScript script;
	script.setName(QStringLiteral("nib-bridge"));
	script.setInjectionPoint(QWebEngineScript::DocumentCreation);
	script.setWorldId(QWebEngineScript::ApplicationWorld);
	script.setRunsOnSubFrames(false);
	script.setSourceCode(focusScript());
	profile->scripts()->insert(script);

	auto *browser = new Browser(profile);

	QObject::connect(profile, &QWebEngineProfile::downloadRequested, browser,
	    [browser](QWebEngineDownloadRequest *dl) {
		dl->accept();
		browser->setStatus(QStringLiteral("downloading %1")
		    .arg(dl->downloadFileName()));
		QObject::connect(dl, &QWebEngineDownloadRequest::isFinishedChanged,
		    browser, [browser, dl] {
			browser->setStatus(QStringLiteral("saved %1")
			    .arg(dl->downloadFileName()));
		});
	});

	if (g_app.enabled) {
		browser->newTab(g_app.home, true);   /* exactly one tab, ever */
	} else {
		bool opened = false;
		for (const QString &a : positional) {
			const QUrl u = resolveInput(a);
			if (u.isValid()) {
				browser->newTab(u, !opened);
				opened = true;
			}
		}
		if (!opened)
			browser->newTab(resolveInput(envOr("NIB_HOME",
			    QStringLiteral("https://duckduckgo.com"))), true);
	}

	browser->show();
	return app.exec();
}

#include "main.moc"
