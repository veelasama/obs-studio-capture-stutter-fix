/******************************************************************************
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasicSettings.hpp"

#include <utility/DisplayRefreshMeasure.hpp>
#include <utility/GameDisplay.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

namespace {
constexpr double kMeasureSeconds = 15.0;
/* Warm-up and temperature wander seen on the reference PC over a week. */
constexpr double kDriftAllowancePpm = 1.5;

QString HoursText(double seconds)
{
	if (!std::isfinite(seconds) || seconds > 99.0 * 3600.0)
		return QStringLiteral("> 99");
	return QString::number(seconds / 3600.0, 'f', 1);
}

QString DisplayName(const std::wstring &device)
{
	return QString::fromStdWString(device).remove(QStringLiteral("\\\\.\\"));
}

/* A word-wrapped label inside the settings scroll area does not grow when
 * its text gets longer; keep its minimum height equal to what it needs. */
class WrappedLabel : public QLabel {
public:
	using QLabel::QLabel;

	void Fit()
	{
		/* Only the real width is meaningful; before the first layout pass
		 * the label is still at its default size. */
		if (!isVisible() || width() <= 0)
			return;
		const int needed = heightForWidth(width());
		if (needed > 0 && needed != minimumHeight())
			setMinimumHeight(needed);
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QLabel::showEvent(event);
		Fit();
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QLabel::resizeEvent(event);
		Fit();
	}
};

void SetWrappedText(QLabel *label, const QString &text)
{
	label->setText(text);
	label->setVisible(!text.isEmpty());
	if (auto *wrapped = dynamic_cast<WrappedLabel *>(label))
		wrapped->Fit();
}

/* Theme classes shared by all OBS themes (see data/themes). */
void SetLabelClass(QLabel *label, const char *classes)
{
	if (label->property("class").toString() == QLatin1String(classes))
		return;
	label->setProperty("class", classes);
	label->style()->unpolish(label);
	label->style()->polish(label);
}
} // namespace

void OBSBasicSettings::SetupDisplayFpsMatch()
{
	matchDisplayDevice = new QComboBox(this);
	matchDisplayDevice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	matchDisplayDevice->setMinimumContentsLength(32);

	matchDisplayButton = new QPushButton(QTStr("Basic.Settings.Video.MatchDisplay.Measure"), this);
	matchDisplayAuto = new QCheckBox(QTStr("Basic.Settings.Video.MatchDisplay.Auto"), this);

	matchDisplayHint = new WrappedLabel(this);
	matchDisplayHint->setWordWrap(true);
	matchDisplayInfo = new WrappedLabel(this);
	matchDisplayInfo->setWordWrap(true);
	matchDisplayInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
	QLabel *note = new WrappedLabel(QTStr("Basic.Settings.Video.MatchDisplay.Note"), this);
	note->setWordWrap(true);
	SetLabelClass(note, "text-small text-muted");

	QWidget *box = new QWidget(this);
	QVBoxLayout *column = new QVBoxLayout(box);
	column->setContentsMargins(0, 0, 0, 0);
	column->setSpacing(6);
	QHBoxLayout *row = new QHBoxLayout();
	row->addWidget(matchDisplayDevice, 1);
	row->addWidget(matchDisplayButton);
	column->addLayout(row);
	column->addWidget(matchDisplayAuto);
	column->addWidget(matchDisplayHint);
	column->addWidget(matchDisplayInfo);
	column->addWidget(note);

	/* Directly below the FPS row. */
	ui->formLayout_15->insertRow(4, QTStr("Basic.Settings.Video.MatchDisplay"), box);

	FillDisplayFpsDevices();

	HookWidget(matchDisplayDevice, &QComboBox::currentIndexChanged, &OBSBasicSettings::VideoChanged);
	HookWidget(matchDisplayAuto, &QCheckBox::toggled, &OBSBasicSettings::VideoChanged);

	connect(matchDisplayButton, &QPushButton::clicked, this, &OBSBasicSettings::MeasureDisplayFps);
	connect(matchDisplayAuto, &QCheckBox::toggled, this, &OBSBasicSettings::UpdateDisplayFpsLock);
	connect(matchDisplayDevice, &QComboBox::currentIndexChanged, this, &OBSBasicSettings::UpdateDisplayFpsHint);

	/* The game may start or move while the dialog is open. */
	QTimer *hintTimer = new QTimer(this);
	connect(hintTimer, &QTimer::timeout, this, &OBSBasicSettings::UpdateDisplayFpsHint);
	hintTimer->start(2000);
}

void OBSBasicSettings::FillDisplayFpsDevices()
{
	const GameDisplay game = FindGameDisplay();
	const bool gameKnown = game.source == GameDisplay::Source::GameWindow;

	const QSignalBlocker blocker(matchDisplayDevice);
	const QString selected = matchDisplayDevice->currentData().toString();
	const bool hadItems = matchDisplayDevice->count() > 0;
	matchDisplayDevice->clear();

	matchDisplayDevice->addItem(QTStr("Basic.Settings.Video.MatchDisplay.AutoMonitor"), QString());
	for (const DisplayRefreshTarget &target : EnumerateRefreshTargets()) {
		QString text = QTStr("Basic.Settings.Video.MatchDisplay.Monitor")
				       .arg(DisplayName(target.device), QString::number(target.nominalHz));
		if (target.primary)
			text += QStringLiteral(" · ") + QTStr("Basic.Settings.Video.MatchDisplay.Primary");
		if (gameKnown && target.device == game.device)
			text += QStringLiteral(" · ◀ ") + QTStr("Basic.Settings.Video.MatchDisplay.GameHere");
		matchDisplayDevice->addItem(text, QString::fromStdWString(target.device));
		matchDisplayDevice->setItemData(matchDisplayDevice->count() - 1,
						QStringLiteral("%1×%2").arg(target.width).arg(target.height),
						Qt::ToolTipRole);
	}

	if (hadItems) {
		const int index = matchDisplayDevice->findData(selected);
		matchDisplayDevice->setCurrentIndex(index >= 0 ? index : 0);
	}


}

std::wstring OBSBasicSettings::SelectedDisplayFpsDevice(bool &automatic) const
{
	const std::wstring selected = matchDisplayDevice->currentData().toString().toStdWString();
	automatic = selected.empty();
	return automatic ? FindGameDisplay().device : selected;
}

void OBSBasicSettings::UpdateDisplayFpsHint()
{
	if (!matchDisplayDevice)
		return;

	const GameDisplay game = FindGameDisplay();

	/* Keep the "◀ game" mark in the list current. */
	QString marked;
	for (int i = 1; i < matchDisplayDevice->count(); i++)
		if (matchDisplayDevice->itemText(i).contains(QStringLiteral("◀")))
			marked = matchDisplayDevice->itemData(i).toString();
	const QString wanted = game.source == GameDisplay::Source::GameWindow ? QString::fromStdWString(game.device)
									      : QString();
	if (marked != wanted)
		FillDisplayFpsDevices();

	const QString selected = matchDisplayDevice->currentData().toString();
	const QString gameName = DisplayName(game.device);
	const QString executable = QString::fromStdWString(game.executable);

	switch (game.source) {
	case GameDisplay::Source::GameWindow:
		if (!selected.isEmpty() && selected.toStdWString() != game.device) {
			SetWrappedText(matchDisplayHint, QTStr("Basic.Settings.Video.MatchDisplay.HintMismatch")
							  .arg(DisplayName(selected.toStdWString()), executable,
							       gameName));
			SetLabelClass(matchDisplayHint, "text-warning");
		} else {
			SetWrappedText(matchDisplayHint, 
				QTStr("Basic.Settings.Video.MatchDisplay.HintGame").arg(executable, gameName));
			SetLabelClass(matchDisplayHint, "text-success");
		}
		break;
	case GameDisplay::Source::Foreground:
		SetWrappedText(matchDisplayHint, 
			QTStr("Basic.Settings.Video.MatchDisplay.HintForeground").arg(executable, gameName));
		SetLabelClass(matchDisplayHint, "text-muted");
		break;
	case GameDisplay::Source::Primary:
		SetWrappedText(matchDisplayHint, QTStr("Basic.Settings.Video.MatchDisplay.HintPrimary").arg(gameName));
		SetLabelClass(matchDisplayHint, "text-warning");
		break;
	}

	if (matchDisplayAuto->isChecked())
		ShowDisplayFpsAutoStatus();
}

void OBSBasicSettings::LoadDisplayFpsMatch()
{
	FillDisplayFpsDevices();
	const char *device = config_get_string(main->Config(), "Video", "FPSMatchDisplayDevice");
	const int index = device && *device ? matchDisplayDevice->findData(QT_UTF8(device)) : 0;
	matchDisplayDevice->setCurrentIndex(index >= 0 ? index : 0);
	matchDisplayAuto->setChecked(config_get_bool(main->Config(), "Video", "FPSMatchDisplayAuto"));
	UpdateDisplayFpsLock();
	UpdateDisplayFpsHint();
}

void OBSBasicSettings::SaveDisplayFpsMatch()
{
	config_set_string(main->Config(), "Video", "FPSMatchDisplayDevice",
			  QT_TO_UTF8(matchDisplayDevice->currentData().toString()));
	config_set_bool(main->Config(), "Video", "FPSMatchDisplayAuto", matchDisplayAuto->isChecked());
}

double OBSBasicSettings::CurrentUiFps() const
{
	switch (ui->fpsType->currentIndex()) {
	case 0: {
		/* Common values are plain numbers; NTSC rates are shown rounded. */
		const double shown = ui->fpsCommon->currentText().toDouble();
		const double ntsc = std::round(shown) * 1000.0 / 1001.0;
		return std::fabs(shown - ntsc) < 0.01 ? ntsc : shown;
	}
	case 1:
		return ui->fpsInteger->value();
	default:
		return ui->fpsDenominator->value() ? (double)ui->fpsNumerator->value() / ui->fpsDenominator->value()
						   : 0.0;
	}
}

void OBSBasicSettings::ShowDisplayFpsResult(const std::wstring &device, double hz, double ppm, double outputFps)
{
	double locked = 0.0;
	if (!DisplayLockedFrameRate(hz, outputFps, locked))
		locked = outputFps;
	const double seconds = SecondsBetweenCorrections(locked, outputFps, kDriftAllowancePpm);
	SetWrappedText(matchDisplayInfo, QTStr("Basic.Settings.Video.MatchDisplay.Result")
					  .arg(DisplayName(device), QString::number(hz, 'f', 6),
					       QString::number(ppm, 'f', 2), QString::number(outputFps, 'f', 6),
					       HoursText(seconds)));
	SetLabelClass(matchDisplayInfo, "");
}

void OBSBasicSettings::ShowDisplayFpsAutoStatus()
{
	bool automatic = false;
	const std::wstring device = SelectedDisplayFpsDevice(automatic);

	double hz = 0.0, ppm = 0.0;
	uint64_t ageMs = 0;
	QString text = QTStr("Basic.Settings.Video.MatchDisplay.AutoInfo");
	if (main->LatestDisplayRefresh(device, hz, ppm, ageMs))
		text += QStringLiteral("\n") + QTStr("Basic.Settings.Video.MatchDisplay.AutoLatest")
						       .arg(DisplayName(device), QString::number(hz, 'f', 6),
							    QString::number(ppm, 'f', 2),
							    QString::number((qulonglong)(ageMs / 1000)));
	else
		text += QStringLiteral("\n") + QTStr("Basic.Settings.Video.MatchDisplay.AutoPending");
	SetWrappedText(matchDisplayInfo, text);
	SetLabelClass(matchDisplayInfo, "");
}

void OBSBasicSettings::UpdateDisplayFpsLock()
{
	const bool automatic = matchDisplayAuto->isChecked();
	ui->fpsType->setEnabled(!automatic);
	ui->fpsTypes->setEnabled(!automatic);
	matchDisplayButton->setEnabled(!automatic);

	if (automatic) {
		ShowDisplayFpsAutoStatus();
	} else {
		SetWrappedText(matchDisplayInfo, QString());
	}
}

void OBSBasicSettings::MeasureDisplayFps()
{
	bool automatic = false;
	const std::wstring device = SelectedDisplayFpsDevice(automatic);
	if (device.empty())
		return;

	auto result = std::make_shared<DisplayRefreshResult>();
	auto done = std::make_shared<std::atomic<bool>>(false);
	auto cancel = std::make_shared<std::atomic<bool>>(false);

	/* Detached: the shared state outlives this dialog if it is closed. */
	std::thread([device, result, done, cancel] {
		*result = MeasureDisplayRefresh(device, kMeasureSeconds, [cancel] { return cancel->load(); });
		done->store(true);
	}).detach();

	const int steps = (int)(kMeasureSeconds * 10.0);
	QProgressDialog *progress =
		new QProgressDialog(QTStr("Basic.Settings.Video.MatchDisplay.Measuring").arg(DisplayName(device)),
				    QTStr("Cancel"), 0, steps, this);
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(0);
	progress->setAttribute(Qt::WA_DeleteOnClose);
	connect(progress, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });

	QTimer *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this, timer, progress, result, done, steps, device]() {
		if (!done->load()) {
			progress->setValue(std::min(progress->value() + 1, steps - 1));
			return;
		}
		timer->stop();
		timer->deleteLater();
		progress->close();

		if (!result->ok) {
			if (result->error != "cancelled") {
				SetWrappedText(matchDisplayInfo, QTStr("Basic.Settings.Video.MatchDisplay.Failed")
								  .arg(QString::fromStdString(result->error)));
				SetLabelClass(matchDisplayInfo, "text-danger");
			}
			return;
		}

		const double current = CurrentUiFps();
		double locked = 0.0;
		if (!DisplayLockedFrameRate(result->hz, current, locked)) {
			SetWrappedText(matchDisplayInfo, QTStr("Basic.Settings.Video.MatchDisplay.NoRatio")
							  .arg(QString::number(result->hz, 'f', 6),
							       QString::number(current, 'f', 3)));
			SetLabelClass(matchDisplayInfo, "text-warning");
			return;
		}

		uint32_t num = 0, den = 0;
		BestFrameRateFraction(locked, 1000000, num, den);
		ui->fpsType->setCurrentIndex(2);
		ui->fpsTypes->setCurrentIndex(2);
		ui->fpsNumerator->setValue((int)num);
		ui->fpsDenominator->setValue((int)den);
		ShowDisplayFpsResult(device, result->hz, result->uncertaintyPpm, (double)num / den);
	});
	timer->start(100);
}
