/*
 * R Markdown -- a Markdown editor that styles the text in place.
 *
 * The document stays plain Markdown: the syntax characters are never hidden
 * or rewritten, they are just dimmed, while what they mark up is styled. So
 * a heading looks like a heading and **bold** looks bold, but the file on
 * disk is exactly what you typed and the caret can always reach every
 * character.
 *
 * Styling is applied as one text_run_array covering the whole document
 * rather than a call per span. BTextView relays out on every styling call, so
 * the per-span approach costs a full relayout per run and is unusable on slow
 * hardware; building the runs first and setting them once is a single pass.
 * The pass itself is deferred until typing pauses.
 *
 * Distributed under the terms of the MIT License.
 */

#include <Alert.h>
#include <Input.h>
#include <Autolock.h>
#include <Directory.h>
#include <Application.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LocaleRoster.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char* const kAppSignature = "application/x-vnd.RMarkdown";

static const uint32 kMsgNew			= 'new ';
static const uint32 kMsgOpen		= 'open';
static const uint32 kMsgSave		= 'save';
static const uint32 kMsgSaveAs		= 'svas';
static const uint32 kMsgRestyle		= 'rstl';
static const uint32 kMsgBigger		= 'big+';
static const uint32 kMsgSmaller		= 'sml-';
static const uint32 kMsgLanguage	= 'lang';
static const uint32 kMsgTheme		= 'thme';

// Long enough that a burst of typing produces one pass instead of one per
// keystroke, short enough that the styling still feels immediate.
static const bigtime_t kRestyleDelay = 180000;

static const float kMinFontSize = 8.0f;
static const float kMaxFontSize = 28.0f;


// #pragma mark - strings


enum string_id {
	kStrFile = 0, kStrNew, kStrOpen, kStrSave, kStrSaveAs, kStrQuit,
	kStrView, kStrBigger, kStrSmaller, kStrDark,
	kStrLanguage, kStrEnglish, kStrKorean,
	kStrUntitled, kStrOpenPanel, kStrSavePanel,
	kStrCannotOpen, kStrCannotSave, kStrModified, kStrStatusFmt,
	kStringCount
};

static const char* const kStringsEn[kStringCount] = {
	"File", "New", "Open" B_UTF8_ELLIPSIS, "Save", "Save as" B_UTF8_ELLIPSIS,
	"Quit",
	"View", "Larger text", "Smaller text", "Dark mode",
	"Language", "English", "Korean",
	"Untitled", "Open Markdown file", "Save Markdown file",
	"Could not open: ", "Could not save: ", " (modified)",
	"%s%s   %" B_PRId32 " words   %" B_PRId32 " characters"
};

static const char* const kStringsKo[kStringCount] = {
	"파일", "새 문서", "열기...", "저장", "다른 이름으로 저장...", "끝내기",
	"보기", "글자 크게", "글자 작게", "다크 모드",
	"언어", "English", "한국어",
	"제목 없음", "마크다운 파일 열기", "마크다운 파일 저장",
	"열 수 없습니다: ", "저장할 수 없습니다: ", " (수정됨)",
	"%s%s   단어 %" B_PRId32 "개   글자 %" B_PRId32 "자"
};

static const char* const* sStrings = kStringsEn;

static inline const char*
T(string_id id)
{
	return sStrings[id];
}


// #pragma mark - styles


enum style_id {
	kStyleBody = 0,
	kStyleMarker,
	kStyleHeading1,
	kStyleHeading2,
	kStyleHeading3,
	kStyleBold,
	kStyleItalic,
	kStyleBoldItalic,
	kStyleCode,
	kStyleQuote,
	kStyleLink,
	kStyleBullet,
	kStyleCount
};


struct style_def {
	BFont		font;
	rgb_color	color;
};

static style_def sStyles[kStyleCount];
static float sBaseSize = 12.0f;
static bool sDark = false;
static rgb_color sPaper = { 255, 255, 255, 255 };


// Rebuilt whenever the base size changes. Everything is derived from the
// user's plain and fixed fonts, so the editor follows the system's font
// choice instead of hardcoding a family.
static void
build_styles()
{
	BFont plain(be_plain_font);
	plain.SetSize(sBaseSize);

	BFont fixed(be_fixed_font);
	fixed.SetSize(sBaseSize);

	// Two palettes rather than one tinted both ways: the dim colour has to
	// stay legible against its own background, and simply inverting the light
	// theme makes the syntax characters vanish into a dark page.
	rgb_color ink    = { 55, 53, 47, 255 };
	rgb_color dim    = { 178, 174, 166, 255 };
	rgb_color code   = { 190, 60, 60, 255 };
	rgb_color quote  = { 110, 108, 102, 255 };
	rgb_color link   = { 45, 105, 175, 255 };
	rgb_color bullet = { 130, 128, 122, 255 };
	rgb_color paper  = { 255, 255, 255, 255 };

	if (sDark) {
		rgb_color darkInk    = { 214, 211, 205, 255 };
		rgb_color darkDim    = { 112, 109, 104, 255 };
		rgb_color darkCode   = { 224, 110, 110, 255 };
		rgb_color darkQuote  = { 150, 147, 141, 255 };
		rgb_color darkLink   = { 110, 165, 225, 255 };
		rgb_color darkBullet = { 140, 137, 131, 255 };
		rgb_color darkPaper  = { 27, 27, 28, 255 };

		ink = darkInk;
		dim = darkDim;
		code = darkCode;
		quote = darkQuote;
		link = darkLink;
		bullet = darkBullet;
		paper = darkPaper;
	}

	sPaper = paper;

	sStyles[kStyleBody].font = plain;
	sStyles[kStyleBody].color = ink;

	// The syntax characters stay visible but step back, which is what keeps
	// the document readable without pretending it is not Markdown.
	sStyles[kStyleMarker].font = plain;
	sStyles[kStyleMarker].color = dim;

	BFont heading(plain);
	heading.SetFace(B_BOLD_FACE);

	heading.SetSize(sBaseSize * 1.7f);
	sStyles[kStyleHeading1].font = heading;
	sStyles[kStyleHeading1].color = ink;

	heading.SetSize(sBaseSize * 1.4f);
	sStyles[kStyleHeading2].font = heading;
	sStyles[kStyleHeading2].color = ink;

	heading.SetSize(sBaseSize * 1.18f);
	sStyles[kStyleHeading3].font = heading;
	sStyles[kStyleHeading3].color = ink;

	BFont bold(plain);
	bold.SetFace(B_BOLD_FACE);
	sStyles[kStyleBold].font = bold;
	sStyles[kStyleBold].color = ink;

	BFont italic(plain);
	italic.SetFace(B_ITALIC_FACE);
	sStyles[kStyleItalic].font = italic;
	sStyles[kStyleItalic].color = ink;

	BFont boldItalic(plain);
	boldItalic.SetFace(B_BOLD_FACE | B_ITALIC_FACE);
	sStyles[kStyleBoldItalic].font = boldItalic;
	sStyles[kStyleBoldItalic].color = ink;

	sStyles[kStyleCode].font = fixed;
	sStyles[kStyleCode].color = code;

	sStyles[kStyleQuote].font = italic;
	sStyles[kStyleQuote].color = quote;

	sStyles[kStyleLink].font = plain;
	sStyles[kStyleLink].color = link;

	sStyles[kStyleBullet].font = bold;
	sStyles[kStyleBullet].color = bullet;
}


// Theme and text size are remembered: a user who picks dark once should not
// have to pick it again, and the size is a readability setting rather than a
// per-document one.
static status_t
settings_path(BPath& path, bool create)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path, create);
	if (status != B_OK)
		return status;

	status = path.Append("RMarkdown");
	if (status != B_OK)
		return status;

	if (create) {
		status = create_directory(path.Path(), 0755);
		if (status != B_OK)
			return status;
	}

	return path.Append("settings");
}


static void
load_settings()
{
	BPath path;
	if (settings_path(path, false) != B_OK)
		return;

	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return;

	BMessage saved;
	if (saved.Unflatten(&file) != B_OK)
		return;

	bool dark = sDark;
	if (saved.FindBool("dark", &dark) == B_OK)
		sDark = dark;

	float size = sBaseSize;
	if (saved.FindFloat("size", &size) == B_OK
		&& size >= kMinFontSize && size <= kMaxFontSize) {
		sBaseSize = size;
	}
}


static void
save_settings()
{
	BPath path;
	if (settings_path(path, true) != B_OK)
		return;

	BMessage saved;
	saved.AddBool("dark", sDark);
	saved.AddFloat("size", sBaseSize);

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK)
		return;

	saved.Flatten(&file);
}


// #pragma mark - parser


// Fills one style byte per character. Working on a flat array and compressing
// it afterwards keeps the rules independent of each other: a block rule can
// paint a whole line and an inline rule can overwrite part of it without
// either having to know about run boundaries.
static void
parse_markdown(const char* text, int32 length, uint8* styles)
{
	memset(styles, kStyleBody, length);

	bool inFence = false;
	int32 line = 0;

	while (line < length) {
		int32 end = line;
		while (end < length && text[end] != '\n')
			end++;

		int32 lineLength = end - line;
		const char* start = text + line;

		// Fenced code. The fence itself is a marker; everything between two
		// fences is code regardless of what it contains.
		if (lineLength >= 3 && strncmp(start, "```", 3) == 0) {
			memset(styles + line, kStyleMarker, lineLength);
			inFence = !inFence;
			line = end + 1;
			continue;
		}
		if (inFence) {
			memset(styles + line, kStyleCode, lineLength);
			line = end + 1;
			continue;
		}

		int32 offset = 0;

		// Heading.
		if (lineLength > 0 && start[0] == '#') {
			int32 hashes = 0;
			while (hashes < lineLength && start[hashes] == '#')
				hashes++;
			if (hashes <= 6 && hashes < lineLength && start[hashes] == ' ') {
				uint8 headingStyle = kStyleHeading1;
				if (hashes == 2)
					headingStyle = kStyleHeading2;
				else if (hashes >= 3)
					headingStyle = kStyleHeading3;

				memset(styles + line, kStyleMarker, hashes + 1);
				memset(styles + line + hashes + 1, headingStyle,
					lineLength - hashes - 1);
				line = end + 1;
				continue;
			}
		}

		// Horizontal rule.
		if (lineLength >= 3) {
			char rule = start[0];
			if (rule == '-' || rule == '*' || rule == '_') {
				bool uniform = true;
				for (int32 i = 0; i < lineLength; i++) {
					if (start[i] != rule && start[i] != ' ') {
						uniform = false;
						break;
					}
				}
				if (uniform) {
					memset(styles + line, kStyleMarker, lineLength);
					line = end + 1;
					continue;
				}
			}
		}

		// Block quote.
		if (lineLength > 0 && start[0] == '>') {
			int32 marker = 1;
			if (marker < lineLength && start[marker] == ' ')
				marker++;
			memset(styles + line, kStyleMarker, marker);
			memset(styles + line + marker, kStyleQuote, lineLength - marker);
			offset = marker;
		} else {
			// List markers, bulleted and numbered.
			int32 indent = 0;
			while (indent < lineLength
				&& (start[indent] == ' ' || start[indent] == '\t')) {
				indent++;
			}

			if (indent + 1 < lineLength
				&& (start[indent] == '-' || start[indent] == '*'
					|| start[indent] == '+')
				&& start[indent + 1] == ' ') {
				styles[line + indent] = kStyleBullet;
				offset = indent + 2;
			} else {
				int32 digits = indent;
				while (digits < lineLength && isdigit(start[digits]))
					digits++;
				if (digits > indent && digits + 1 < lineLength
					&& start[digits] == '.' && start[digits + 1] == ' ') {
					memset(styles + line + indent, kStyleBullet,
						digits - indent + 1);
					offset = digits + 2;
				}
			}
		}

		// Inline spans. Everything here works on the remainder of the line,
		// so a bold run inside a list item or a quote is styled the same way
		// it would be in a plain paragraph.
		while (offset < lineLength) {
			char c = start[offset];

			if (c == '`') {
				int32 close = offset + 1;
				while (close < lineLength && start[close] != '`')
					close++;
				if (close < lineLength) {
					styles[line + offset] = kStyleMarker;
					styles[line + close] = kStyleMarker;
					for (int32 i = offset + 1; i < close; i++)
						styles[line + i] = kStyleCode;
					offset = close + 1;
					continue;
				}
			} else if (c == '*' || c == '_') {
				bool doubled = offset + 1 < lineLength
					&& start[offset + 1] == c;
				int32 markerLength = doubled ? 2 : 1;
				int32 close = offset + markerLength;

				while (close < lineLength) {
					if (start[close] == c
						&& (!doubled
							|| (close + 1 < lineLength
								&& start[close + 1] == c))) {
						break;
					}
					close++;
				}

				// An unmatched marker is left as ordinary text rather than
				// styling the rest of the line on a guess.
				if (close < lineLength && close > offset + markerLength) {
					uint8 inner = doubled ? kStyleBold : kStyleItalic;
					for (int32 i = 0; i < markerLength; i++) {
						styles[line + offset + i] = kStyleMarker;
						styles[line + close + i] = kStyleMarker;
					}
					for (int32 i = offset + markerLength; i < close; i++) {
						if (styles[line + i] == kStyleBody
							|| styles[line + i] == kStyleQuote) {
							styles[line + i] = inner;
						} else if (styles[line + i] == kStyleBold
							|| styles[line + i] == kStyleItalic) {
							styles[line + i] = kStyleBoldItalic;
						}
					}
					offset = close + markerLength;
					continue;
				}
			} else if (c == '[') {
				int32 close = offset + 1;
				while (close < lineLength && start[close] != ']')
					close++;
				if (close < lineLength && close + 1 < lineLength
					&& start[close + 1] == '(') {
					int32 paren = close + 2;
					while (paren < lineLength && start[paren] != ')')
						paren++;
					if (paren < lineLength) {
						styles[line + offset] = kStyleMarker;
						for (int32 i = offset + 1; i < close; i++)
							styles[line + i] = kStyleLink;
						for (int32 i = close; i <= paren; i++)
							styles[line + i] = kStyleMarker;
						offset = paren + 1;
						continue;
					}
				}
			}

			offset++;
		}

		line = end + 1;
	}
}


// #pragma mark - MarkdownView


class MarkdownView : public BTextView {
public:
							MarkdownView(BRect frame);

	virtual	void			InsertText(const char* text, int32 length,
								int32 offset, const text_run_array* runs);
	virtual	void			DeleteText(int32 start, int32 finish);
	virtual	void			MessageReceived(BMessage* message);

			void			Restyle();
			void			ScheduleRestyle();
			void			LoadText(const char* text);
			void			SetChangeTarget(BHandler* target)
								{ fTarget = target; }

private:
			BHandler*		fTarget;
			bool			fSuppress;
			// True between B_INPUT_METHOD_STARTED and _STOPPED.
			bool			fComposing;
};


MarkdownView::MarkdownView(BRect frame)
	:
	BTextView(frame, "markdown", BRect(0, 0, frame.Width() - 16,
		frame.Height()), B_FOLLOW_ALL, B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS),
	fTarget(NULL),
	fSuppress(false),
	fComposing(false)
{
	SetStylable(true);
	SetWordWrap(true);
	SetInsets(12, 10, 12, 10);
}


void
MarkdownView::InsertText(const char* text, int32 length, int32 offset,
	const text_run_array* runs)
{
	BTextView::InsertText(text, length, offset, runs);
	if (!fSuppress)
		ScheduleRestyle();
}


void
MarkdownView::DeleteText(int32 start, int32 finish)
{
	BTextView::DeleteText(start, finish);
	if (!fSuppress)
		ScheduleRestyle();
}


// Replacing the whole document is not an edit, so it must not schedule the
// deferred pass -- that pass is what marks the document modified, and opening
// a file would have flagged it dirty before the user had touched anything.
void
MarkdownView::LoadText(const char* text)
{
	fSuppress = true;
	BTextView::SetText(text);
	fSuppress = false;
	Restyle();
}


// Restyling replaces the run array for the whole document, and BTextView
// keeps the half-finished syllable of an input method as inline text with a
// style of its own. Rewriting every style underneath it destroys that state:
// with a Korean keyboard the composing character was dropped or frozen on
// whichever keystroke happened to fall after the restyle timer, which reads
// as "Korean does not work" rather than as a styling bug.
//
// The pass is held back until the input method says it has finished, then run
// once.
void
MarkdownView::MessageReceived(BMessage* message)
{
	if (message->what == B_INPUT_METHOD_EVENT) {
		int32 opcode = 0;
		if (message->FindInt32("be:opcode", &opcode) == B_OK) {
			if (opcode == B_INPUT_METHOD_STARTED)
				fComposing = true;
			else if (opcode == B_INPUT_METHOD_STOPPED) {
				fComposing = false;
				ScheduleRestyle();
			}
		}
	}

	BTextView::MessageReceived(message);
}


void
MarkdownView::ScheduleRestyle()
{
	if (fComposing)
		return;

	if (fTarget != NULL && fTarget->Looper() != NULL)
		BMessenger(fTarget).SendMessage(kMsgRestyle);
}


void
MarkdownView::Restyle()
{
	int32 length = TextLength();
	if (length <= 0)
		return;

	const char* text = Text();
	uint8* styles = (uint8*)malloc(length);
	if (styles == NULL)
		return;

	parse_markdown(text, length, styles);

	// Compress to runs. BTextView wants strictly increasing offsets, so
	// identical neighbours have to be merged before the array is built.
	int32 runCount = 1;
	for (int32 i = 1; i < length; i++) {
		if (styles[i] != styles[i - 1])
			runCount++;
	}

	int32 size = 0;
	text_run_array* runs = BTextView::AllocRunArray(runCount, &size);
	if (runs == NULL) {
		free(styles);
		return;
	}

	int32 index = 0;
	runs->runs[0].offset = 0;
	runs->runs[0].font = sStyles[styles[0]].font;
	runs->runs[0].color = sStyles[styles[0]].color;

	for (int32 i = 1; i < length; i++) {
		if (styles[i] == styles[i - 1])
			continue;
		index++;
		runs->runs[index].offset = i;
		runs->runs[index].font = sStyles[styles[i]].font;
		runs->runs[index].color = sStyles[styles[i]].color;
	}
	runs->count = index + 1;

	// SetRunArray re-enters InsertText on some paths; without this the
	// styling pass would schedule another styling pass forever.
	fSuppress = true;
	SetRunArray(0, length, runs);
	fSuppress = false;

	BTextView::FreeRunArray(runs);
	free(styles);
}


// #pragma mark - EditorWindow


class EditorWindow : public BWindow {
public:
							EditorWindow();

			void			Open(const entry_ref& ref) { _Load(ref); }

	virtual	void			MessageReceived(BMessage* message);
	virtual	bool			QuitRequested();

private:
			void			_New();
			void			_Load(const entry_ref& ref);
			void			_Save(const char* path);
			void			_UpdateStatus();
			void			_ApplyLanguage(bool korean);
			void			_SetFontSize(float size);
			void			_ApplyTheme();

			MarkdownView*	fText;
			BView*			fFooter;
			BStringView*	fStatus;
			BFilePanel*		fOpenPanel;
			BFilePanel*		fSavePanel;
			BMessageRunner*	fRestyleRunner;
			BString			fPath;
			bool			fDirty;

			BMenu*			fFileMenu;
			BMenu*			fViewMenu;
			BMenu*			fLanguageMenu;
			BMenuItem*		fNewItem;
			BMenuItem*		fOpenItem;
			BMenuItem*		fSaveItem;
			BMenuItem*		fSaveAsItem;
			BMenuItem*		fQuitItem;
			BMenuItem*		fBiggerItem;
			BMenuItem*		fSmallerItem;
			BMenuItem*		fDarkItem;
			BMenuItem*		fEnglishItem;
			BMenuItem*		fKoreanItem;
};


EditorWindow::EditorWindow()
	:
	BWindow(BRect(80, 80, 900, 620), "R Markdown", B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS),
	fText(NULL),
	fStatus(NULL),
	fOpenPanel(NULL),
	fSavePanel(NULL),
	fRestyleRunner(NULL),
	fDirty(false)
{
	BRect bounds = Bounds();

	BMenuBar* menuBar = new BMenuBar(BRect(0, 0, bounds.right, 18), "menu");

	fFileMenu = new BMenu(T(kStrFile));
	fFileMenu->AddItem(fNewItem
		= new BMenuItem(T(kStrNew), new BMessage(kMsgNew), 'N'));
	fFileMenu->AddItem(fOpenItem
		= new BMenuItem(T(kStrOpen), new BMessage(kMsgOpen), 'O'));
	fFileMenu->AddItem(fSaveItem
		= new BMenuItem(T(kStrSave), new BMessage(kMsgSave), 'S'));
	fFileMenu->AddItem(fSaveAsItem
		= new BMenuItem(T(kStrSaveAs), new BMessage(kMsgSaveAs)));
	fFileMenu->AddSeparatorItem();
	fFileMenu->AddItem(fQuitItem
		= new BMenuItem(T(kStrQuit), new BMessage(B_QUIT_REQUESTED), 'Q'));
	menuBar->AddItem(fFileMenu);

	fViewMenu = new BMenu(T(kStrView));
	fViewMenu->AddItem(fBiggerItem
		= new BMenuItem(T(kStrBigger), new BMessage(kMsgBigger), '+'));
	fViewMenu->AddItem(fSmallerItem
		= new BMenuItem(T(kStrSmaller), new BMessage(kMsgSmaller), '-'));
	fViewMenu->AddSeparatorItem();
	fViewMenu->AddItem(fDarkItem
		= new BMenuItem(T(kStrDark), new BMessage(kMsgTheme), 'D'));
	fDarkItem->SetMarked(sDark);
	menuBar->AddItem(fViewMenu);

	fLanguageMenu = new BMenu(T(kStrLanguage));
	fLanguageMenu->SetRadioMode(true);
	BMessage* english = new BMessage(kMsgLanguage);
	english->AddBool("korean", false);
	BMessage* korean = new BMessage(kMsgLanguage);
	korean->AddBool("korean", true);
	fLanguageMenu->AddItem(fEnglishItem
		= new BMenuItem(T(kStrEnglish), english));
	fLanguageMenu->AddItem(fKoreanItem
		= new BMenuItem(T(kStrKorean), korean));
	(sStrings == kStringsKo ? fKoreanItem : fEnglishItem)->SetMarked(true);
	menuBar->AddItem(fLanguageMenu);

	AddChild(menuBar);

	BRect textFrame = bounds;
	textFrame.top = menuBar->Bounds().bottom + 1;
	textFrame.bottom -= 22;
	textFrame.right -= B_V_SCROLL_BAR_WIDTH;

	fText = new MarkdownView(textFrame);
	fText->SetChangeTarget(this);

	BScrollView* scroll = new BScrollView("scroll", fText,
		B_FOLLOW_ALL, 0, false, true, B_PLAIN_BORDER);
	AddChild(scroll);

	// The status line sits in its own strip rather than directly on the
	// window: a BStringView only paints behind its own text, so in the dark
	// theme the panel colour would show through everywhere around it.
	fFooter = new BView(BRect(0, bounds.bottom - 21, bounds.right,
		bounds.bottom), "footer", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_BOTTOM,
		B_WILL_DRAW);
	AddChild(fFooter);

	fStatus = new BStringView(BRect(10, 3, bounds.right - 10, 19), "status",
		"", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP);
	fFooter->AddChild(fStatus);

	fText->MakeFocus(true);
	_ApplyTheme();
	_UpdateStatus();
}


void
EditorWindow::_SetFontSize(float size)
{
	if (size < kMinFontSize || size > kMaxFontSize)
		return;

	sBaseSize = size;
	build_styles();
	_ApplyTheme();
	fText->Restyle();
	save_settings();
	_UpdateStatus();
}


// The page colour lives on the view, not in the run array, so it has to be
// pushed separately whenever the palette changes.
void
EditorWindow::_ApplyTheme()
{
	fText->SetViewColor(sPaper);
	fText->SetLowColor(sPaper);
	fText->Invalidate();

	// A shade off the page so the strip reads as a separate band in both
	// themes, rather than as the bottom of the document.
	rgb_color strip = sDark
		? tint_color(sPaper, B_LIGHTEN_1_TINT)
		: tint_color(sPaper, B_DARKEN_1_TINT);
	// The quote colour is deliberately faint against the page; on the strip
	// it was too faint to read, so the label follows the body text instead.
	rgb_color label = sStyles[kStyleBody].color;

	fFooter->SetViewColor(strip);
	fFooter->SetLowColor(strip);
	fFooter->Invalidate();

	fStatus->SetViewColor(strip);
	fStatus->SetLowColor(strip);
	fStatus->SetHighColor(label);
	fStatus->Invalidate();

	fDarkItem->SetMarked(sDark);
}


void
EditorWindow::_New()
{
	fText->LoadText("");
	fPath = "";
	fDirty = false;
	_UpdateStatus();
}


void
EditorWindow::_Load(const entry_ref& ref)
{
	BPath path(&ref);
	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() != B_OK) {
		BString message(T(kStrCannotOpen));
		message << strerror(file.InitCheck());
		BAlert* alert = new BAlert("R Markdown", message.String(), "OK");
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go(NULL);
		return;
	}

	off_t size = 0;
	file.GetSize(&size);
	if (size < 0 || size > 8 * 1024 * 1024)
		size = 8 * 1024 * 1024;

	char* buffer = (char*)malloc(size + 1);
	if (buffer == NULL)
		return;

	ssize_t read = file.Read(buffer, size);
	if (read < 0)
		read = 0;
	buffer[read] = '\0';

	fText->LoadText(buffer);
	free(buffer);

	fPath = path.Path();
	fDirty = false;
	_UpdateStatus();
}


void
EditorWindow::_Save(const char* path)
{
	BFile file(path, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK) {
		BString message(T(kStrCannotSave));
		message << strerror(file.InitCheck());
		BAlert* alert = new BAlert("R Markdown", message.String(), "OK");
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go(NULL);
		return;
	}

	const char* text = fText->Text();
	file.Write(text, fText->TextLength());

	fPath = path;
	fDirty = false;
	_UpdateStatus();
}


void
EditorWindow::_UpdateStatus()
{
	const char* text = fText->Text();
	int32 length = fText->TextLength();

	int32 words = 0;
	bool inWord = false;
	for (int32 i = 0; i < length; i++) {
		bool space = text[i] == ' ' || text[i] == '\n' || text[i] == '\t';
		if (!space && !inWord)
			words++;
		inWord = !space;
	}

	BString name(T(kStrUntitled));
	if (fPath.Length() > 0) {
		int32 slash = fPath.FindLast('/');
		name = slash >= 0 ? fPath.String() + slash + 1 : fPath.String();
	}

	char status[512];
	snprintf(status, sizeof(status), T(kStrStatusFmt), name.String(),
		fDirty ? T(kStrModified) : "", words, length);
	fStatus->SetText(status);

	BString title("R Markdown - ");
	title << name;
	SetTitle(title.String());
}


void
EditorWindow::_ApplyLanguage(bool korean)
{
	sStrings = korean ? kStringsKo : kStringsEn;

	fFileMenu->Superitem()->SetLabel(T(kStrFile));
	fNewItem->SetLabel(T(kStrNew));
	fOpenItem->SetLabel(T(kStrOpen));
	fSaveItem->SetLabel(T(kStrSave));
	fSaveAsItem->SetLabel(T(kStrSaveAs));
	fQuitItem->SetLabel(T(kStrQuit));

	fViewMenu->Superitem()->SetLabel(T(kStrView));
	fBiggerItem->SetLabel(T(kStrBigger));
	fSmallerItem->SetLabel(T(kStrSmaller));
	fDarkItem->SetLabel(T(kStrDark));

	fLanguageMenu->Superitem()->SetLabel(T(kStrLanguage));
	fEnglishItem->SetLabel(T(kStrEnglish));
	fKoreanItem->SetLabel(T(kStrKorean));
	(korean ? fKoreanItem : fEnglishItem)->SetMarked(true);

	_UpdateStatus();
}


void
EditorWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgRestyle:
		{
			fDirty = true;
			// One-shot: the runner is replaced on every keystroke, so a burst
			// of typing collapses into a single pass once it stops.
			delete fRestyleRunner;
			fRestyleRunner = new BMessageRunner(BMessenger(this),
				new BMessage('rsdo'), kRestyleDelay, 1);
			break;
		}

		case 'rsdo':
			delete fRestyleRunner;
			fRestyleRunner = NULL;
			fText->Restyle();
			_UpdateStatus();
			break;

		case kMsgNew:
			_New();
			break;

		case kMsgOpen:
			if (fOpenPanel == NULL) {
				fOpenPanel = new BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), NULL, 0, false);
			}
			fOpenPanel->Window()->SetTitle(T(kStrOpenPanel));
			fOpenPanel->Show();
			break;

		case B_REFS_RECEIVED:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) == B_OK)
				_Load(ref);
			break;
		}

		case kMsgSave:
			if (fPath.Length() > 0) {
				_Save(fPath.String());
				break;
			}
			// falls through to Save as when there is no path yet

		case kMsgSaveAs:
			if (fSavePanel == NULL) {
				fSavePanel = new BFilePanel(B_SAVE_PANEL,
					new BMessenger(this), NULL, 0, false);
			}
			fSavePanel->Window()->SetTitle(T(kStrSavePanel));
			fSavePanel->SetSaveText("Untitled.md");
			fSavePanel->Show();
			break;

		case B_SAVE_REQUESTED:
		{
			entry_ref directory;
			const char* name = NULL;
			if (message->FindRef("directory", &directory) == B_OK
				&& message->FindString("name", &name) == B_OK) {
				BPath path(&directory);
				path.Append(name);
				_Save(path.Path());
			}
			break;
		}

		case kMsgBigger:
			_SetFontSize(sBaseSize + 1);
			break;

		case kMsgSmaller:
			_SetFontSize(sBaseSize - 1);
			break;

		case kMsgTheme:
			sDark = !sDark;
			build_styles();
			_ApplyTheme();
			fText->Restyle();
			save_settings();
			break;

		case kMsgLanguage:
		{
			bool korean = false;
			message->FindBool("korean", &korean);
			_ApplyLanguage(korean);
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
EditorWindow::QuitRequested()
{
	delete fRestyleRunner;
	fRestyleRunner = NULL;
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


// #pragma mark -


static void
choose_language()
{
	BMessage preferred;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&preferred) != B_OK)
		return;

	const char* language = NULL;
	for (int32 i = 0;
			preferred.FindString("language", i, &language) == B_OK; i++) {
		if (language != NULL && strncmp(language, "ko", 2) == 0) {
			sStrings = kStringsKo;
			return;
		}
	}
}


// Files arrive two ways: as arguments when started from a shell, and as refs
// when opened from Tracker or handed to a copy that is already running. Both
// end up at the same window, so the second one does not open a second editor.
class MarkdownApp : public BApplication {
public:
	MarkdownApp()
		:
		BApplication(kAppSignature),
		fWindow(NULL),
		fHavePending(false)
	{
	}

	virtual void ReadyToRun()
	{
		if (fWindow == NULL) {
			fWindow = new EditorWindow();
			fWindow->Show();
		}

		if (fHavePending) {
			fHavePending = false;
			_OpenLocked(fPending);
		}
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		if (argc < 2)
			return;

		entry_ref ref;
		if (get_ref_for_path(argv[1], &ref) != B_OK)
			return;

		if (fWindow != NULL)
			_OpenLocked(ref);
		else {
			fPending = ref;
			fHavePending = true;
		}
	}

	virtual void RefsReceived(BMessage* message)
	{
		entry_ref ref;
		if (message->FindRef("refs", &ref) != B_OK)
			return;

		if (fWindow != NULL) {
			_OpenLocked(ref);
			fWindow->Activate(true);
		} else {
			fPending = ref;
			fHavePending = true;
		}
	}

private:
	// Every one of these runs on the application thread, which may not touch
	// another looper's views: doing so tripped "Looper must be locked" on the
	// first file opened from the command line.
	void _OpenLocked(const entry_ref& ref)
	{
		if (fWindow == NULL || !fWindow->Lock())
			return;
		fWindow->Open(ref);
		fWindow->Unlock();
	}

			EditorWindow*	fWindow;
			entry_ref		fPending;
			bool			fHavePending;
};


int
main(void)
{
	MarkdownApp app;

	choose_language();
	sBaseSize = be_plain_font->Size();

	// Start in whichever theme the desktop is using: if the system's document
	// background is dark, a white page would be the odd one out.
	rgb_color background = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	sDark = (background.red + background.green + background.blue) < 3 * 128;

	// A saved choice wins over the desktop's, since it was made deliberately.
	load_settings();

	build_styles();

	app.Run();
	return 0;
}
