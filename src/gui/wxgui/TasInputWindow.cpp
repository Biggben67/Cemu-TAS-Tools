#include "wxgui/TasInputWindow.h"

#include "wxgui/input/HotkeySettings.h"
#include "input/InputManager.h"
#include "input/TAS/TASInput.h"
#include <wx/dcbuffer.h>
#include <wx/slider.h>
#include <array>
#include <functional>

namespace
{
	wxSpinCtrl* CreateStickSpin(wxWindow* parent)
	{
		auto* spin = new wxSpinCtrl(parent, wxID_ANY);
		spin->SetRange(0, 255);
		return spin;
	}

	wxSlider* CreateStickSlider(wxWindow* parent, bool vertical = false)
	{
		const long style = vertical ? wxSL_VERTICAL : wxSL_HORIZONTAL;
		return new wxSlider(parent, wxID_ANY, 128, 0, 255, wxDefaultPosition, wxDefaultSize, style);
	}

	class TasStickPad final : public wxPanel
	{
	public:
		using OnChangeFn = std::function<void(float, float)>;

		TasStickPad(wxWindow* parent, OnChangeFn onChange)
			: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(220, 220), wxBORDER_SIMPLE), m_onChange(std::move(onChange))
		{
			SetMinSize(wxSize(160, 160));
			SetBackgroundStyle(wxBG_STYLE_PAINT);
			Bind(wxEVT_PAINT, &TasStickPad::OnPaint, this);
			Bind(wxEVT_LEFT_DOWN, &TasStickPad::OnMouseDown, this);
			Bind(wxEVT_LEFT_UP, &TasStickPad::OnMouseUp, this);
			Bind(wxEVT_RIGHT_DOWN, &TasStickPad::OnMouseRightDown, this);
			Bind(wxEVT_MOTION, &TasStickPad::OnMouseMove, this);
			Bind(wxEVT_KEY_DOWN, &TasStickPad::OnKeyDown, this);
			Bind(wxEVT_MOUSE_CAPTURE_LOST, &TasStickPad::OnCaptureLost, this);
			Bind(wxEVT_DESTROY, &TasStickPad::OnWindowDestroy, this);
		}

		void SetValue(float x, float y)
		{
			m_x = std::clamp(x, -1.0f, 1.0f);
			m_y = std::clamp(y, -1.0f, 1.0f);
			Refresh();
		}

	private:
		void OnPaint(wxPaintEvent&)
		{
			wxAutoBufferedPaintDC dc(this);
			dc.Clear();

			const wxSize sz = GetClientSize();
			const int cx = sz.GetWidth() / 2;
			const int cy = sz.GetHeight() / 2;
			const int halfSize = std::max(8, std::min(sz.GetWidth(), sz.GetHeight()) / 2 - 10);

			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(wxColour(96, 96, 96)));
			dc.DrawRectangle(cx - halfSize, cy - halfSize, halfSize * 2, halfSize * 2);

			dc.SetPen(wxPen(*wxWHITE, 2));
			dc.SetBrush(*wxTRANSPARENT_BRUSH);
			dc.DrawCircle(cx, cy, halfSize);

			dc.SetPen(wxPen(wxColour(170, 170, 170)));
			dc.DrawLine(cx - halfSize, cy, cx + halfSize, cy);
			dc.DrawLine(cx, cy - halfSize, cx, cy + halfSize);

			const int knobX = cx + (int)std::lround(m_x * (float)halfSize);
			const int knobY = cy - (int)std::lround(m_y * (float)halfSize);
			dc.SetPen(*wxBLACK_PEN);
			dc.SetBrush(*wxBLUE_BRUSH);
			dc.DrawCircle(knobX, knobY, std::max(4, halfSize / 12));
		}

		void UpdateFromMouse(const wxPoint& p, bool emitChange)
		{
			const wxSize sz = GetClientSize();
			const float cx = (float)sz.GetWidth() * 0.5f;
			const float cy = (float)sz.GetHeight() * 0.5f;
			const float halfSize = (float)std::max(8, std::min(sz.GetWidth(), sz.GetHeight()) / 2 - 10);
			const float x = std::clamp(((float)p.x - cx) / halfSize, -1.0f, 1.0f);
			const float y = std::clamp((cy - (float)p.y) / halfSize, -1.0f, 1.0f);

			m_x = x;
			m_y = y;
			Refresh();
			if (emitChange && m_onChange)
				m_onChange(m_x, m_y);
		}

		void OnMouseDown(wxMouseEvent& event)
		{
			SetFocus();
			m_dragging = true;
			if (!HasCapture())
				CaptureMouse();
			UpdateFromMouse(event.GetPosition(), true);
		}

		void OnMouseUp(wxMouseEvent& event)
		{
			if (m_dragging)
			{
				m_dragging = false;
				if (HasCapture())
					ReleaseMouse();
			}
			UpdateFromMouse(event.GetPosition(), true);
		}

		void OnMouseRightDown(wxMouseEvent&)
		{
			m_x = 0.0f;
			m_y = 0.0f;
			Refresh();
			if (m_onChange)
				m_onChange(m_x, m_y);
		}

		void OnMouseMove(wxMouseEvent& event)
		{
			if (m_dragging && event.LeftIsDown())
				UpdateFromMouse(event.GetPosition(), true);
		}

		void OnCaptureLost(wxMouseCaptureLostEvent&)
		{
			m_dragging = false;
		}

		void OnWindowDestroy(wxWindowDestroyEvent& event)
		{
			m_dragging = false;
			if (HasCapture())
				ReleaseMouse();
			event.Skip();
		}

		void OnKeyDown(wxKeyEvent& event)
		{
			if (HotkeySettings::CaptureInput(event))
			{
				event.Skip(false);
				return;
			}
			// Don't let focused stick pad emit system beeps for unrelated keys.
			event.Skip(false);
		}

	private:
		OnChangeFn m_onChange;
		float m_x{};
		float m_y{};
		bool m_dragging{};
	};
}

TasInputWindow::TasInputWindow(wxFrame* parent)
	: wxFrame(parent, wxID_ANY, _("TAS Input Editor"), wxDefaultPosition, wxSize(900, 720), wxDEFAULT_FRAME_STYLE | wxTAB_TRAVERSAL),
	  m_stateSyncTimer(this)
{
	auto* panel = new wxPanel(this);
	auto* root = new wxBoxSizer(wxVERTICAL);

	auto* topRow = new wxBoxSizer(wxHORIZONTAL);
	topRow->Add(new wxStaticText(panel, wxID_ANY, _("Player")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	m_playerChoice = new wxChoice(panel, wxID_ANY);
	for (size_t i = 0; i < InputManager::kMaxVPADControllers; ++i)
		m_playerChoice->Append(wxString::Format("%zu", i + 1));
	m_playerChoice->SetSelection(0);
	topRow->Add(m_playerChoice, 0, wxRIGHT, 12);

	m_enableControllerInputCheck = new wxCheckBox(panel, wxID_ANY, _("Enable Controller Input"));
	topRow->Add(m_enableControllerInputCheck, 1, wxALIGN_CENTER_VERTICAL);
	root->Add(topRow, 0, wxALL | wxEXPAND, 8);

	auto* axesBox = new wxStaticBoxSizer(wxVERTICAL, panel, _("Sticks"));

	auto* stickPadRow = new wxBoxSizer(wxHORIZONTAL);
	auto* leftPadCol = new wxBoxSizer(wxVERTICAL);
	auto* rightPadCol = new wxBoxSizer(wxVERTICAL);
	leftPadCol->Add(new wxStaticText(panel, wxID_ANY, _("Left Stick (Main)")), 0, wxBOTTOM, 4);
	rightPadCol->Add(new wxStaticText(panel, wxID_ANY, _("Right Stick")), 0, wxBOTTOM, 4);

	auto addStickTopXControl = [&](wxBoxSizer* col, wxSpinCtrl** xSpin, wxSlider** xSlider)
	{
		auto* xRow = new wxBoxSizer(wxHORIZONTAL);
		xRow->Add(new wxStaticText(panel, wxID_ANY, _("X")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		*xSlider = CreateStickSlider(panel, false);
		*xSpin = CreateStickSpin(panel);
		(*xSpin)->SetMinSize(wxSize(70, -1));
		xRow->Add(*xSlider, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
		xRow->Add(*xSpin, 0, wxALIGN_CENTER_VERTICAL);
		col->Add(xRow, 0, wxBOTTOM | wxEXPAND, 4);
	};

	addStickTopXControl(leftPadCol, &m_lx, &m_lxSlider);
	addStickTopXControl(rightPadCol, &m_rx, &m_rxSlider);
	m_lySlider = CreateStickSlider(panel, true);
	m_lySlider->SetMinSize(wxSize(40, 150));
	m_ly = CreateStickSpin(panel);
	m_ly->SetMinSize(wxSize(70, -1));
	m_rySlider = CreateStickSlider(panel, true);
	m_rySlider->SetMinSize(wxSize(40, 150));
	m_ry = CreateStickSpin(panel);
	m_ry->SetMinSize(wxSize(70, -1));

	m_leftStickPad = new TasStickPad(panel, [this](float x, float y)
	{
		if (m_updatingUi)
			return;
		m_updatingUi = true;
		SyncStickControlsFromPad(true, x, y);
		m_updatingUi = false;
		PushStateToTas();
	});
	m_rightStickPad = new TasStickPad(panel, [this](float x, float y)
	{
		if (m_updatingUi)
			return;
		m_updatingUi = true;
		SyncStickControlsFromPad(false, x, y);
		m_updatingUi = false;
		PushStateToTas();
	});
	auto* leftPadAndY = new wxBoxSizer(wxHORIZONTAL);
	leftPadAndY->Add(m_leftStickPad, 1, wxRIGHT | wxEXPAND, 6);
	auto* leftYCol = new wxBoxSizer(wxVERTICAL);
	leftYCol->Add(new wxStaticText(panel, wxID_ANY, _("Y")), 0, wxBOTTOM, 4);
	leftYCol->Add(m_lySlider, 1, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 4);
	leftYCol->Add(m_ly, 0, wxALIGN_CENTER_HORIZONTAL);
	leftPadAndY->Add(leftYCol, 0, wxEXPAND);
	leftPadCol->Add(leftPadAndY, 1, wxEXPAND);

	auto* rightPadAndY = new wxBoxSizer(wxHORIZONTAL);
	rightPadAndY->Add(m_rightStickPad, 1, wxRIGHT | wxEXPAND, 6);
	auto* rightYCol = new wxBoxSizer(wxVERTICAL);
	rightYCol->Add(new wxStaticText(panel, wxID_ANY, _("Y")), 0, wxBOTTOM, 4);
	rightYCol->Add(m_rySlider, 1, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 4);
	rightYCol->Add(m_ry, 0, wxALIGN_CENTER_HORIZONTAL);
	rightPadAndY->Add(rightYCol, 0, wxEXPAND);
	rightPadCol->Add(rightPadAndY, 1, wxEXPAND);

	m_leftStickValue = new wxStaticText(panel, wxID_ANY, _("X: 128  Y: 128"));
	m_rightStickValue = new wxStaticText(panel, wxID_ANY, _("X: 128  Y: 128"));
	leftPadCol->Add(m_leftStickValue, 0, wxTOP, 4);
	rightPadCol->Add(m_rightStickValue, 0, wxTOP, 4);

	stickPadRow->Add(leftPadCol, 1, wxRIGHT | wxEXPAND, 10);
	stickPadRow->Add(rightPadCol, 1, wxEXPAND);
	axesBox->Add(stickPadRow, 1, wxALL | wxEXPAND, 4);

	root->Add(axesBox, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

	auto* buttonsBox = new wxStaticBoxSizer(wxVERTICAL, panel, _("Buttons"));
	auto* buttonsGrid = new wxGridSizer(0, 4, 4, 8);
	const std::array<std::pair<const char*, uint32>, 17> buttonDefs{{
		{"A", TasInput::kBtnA}, {"B", TasInput::kBtnB}, {"X", TasInput::kBtnX}, {"Y", TasInput::kBtnY},
		{"L", TasInput::kBtnL}, {"R", TasInput::kBtnR}, {"ZL", TasInput::kBtnZL}, {"ZR", TasInput::kBtnZR},
		{"Plus", TasInput::kBtnPlus}, {"Minus", TasInput::kBtnMinus}, {"Up", TasInput::kBtnUp}, {"Down", TasInput::kBtnDown},
		{"Left", TasInput::kBtnLeft}, {"Right", TasInput::kBtnRight}, {"StickL", TasInput::kBtnStickL}, {"StickR", TasInput::kBtnStickR},
		{"Home", TasInput::kBtnHome},
	}};

	for (const auto& [label, mask] : buttonDefs)
	{
		auto* checkbox = new wxCheckBox(panel, wxID_ANY, wxString::FromUTF8(label));
		checkbox->SetName(wxString::FromUTF8(label));
		checkbox->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnButtonRightClick, this);
		m_buttonChecks.emplace_back(mask, checkbox);
		buttonsGrid->Add(checkbox, 0, wxEXPAND);
	}
	buttonsBox->Add(buttonsGrid, 1, wxALL | wxEXPAND, 4);

	auto* turboRow = new wxBoxSizer(wxHORIZONTAL);
	turboRow->Add(new wxStaticText(panel, wxID_ANY, _("Turbo interval (frames)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
	m_turboInterval = new wxSpinCtrl(panel, wxID_ANY);
	m_turboInterval->SetRange(1, 60);
	m_turboInterval->SetValue(2);
	turboRow->Add(m_turboInterval, 0);
	buttonsBox->Add(turboRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);

	root->Add(buttonsBox, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

	auto* bottomRow = new wxBoxSizer(wxHORIZONTAL);
	bottomRow->AddStretchSpacer();
	auto* resetButton = new wxButton(panel, wxID_ANY, _("Reset values"));
	bottomRow->Add(resetButton, 0);
	root->Add(bottomRow, 0, wxALL | wxEXPAND, 8);

	panel->SetSizer(root);

	m_playerChoice->Bind(wxEVT_CHOICE, &TasInputWindow::OnPlayerChanged, this);
	m_enableControllerInputCheck->Bind(wxEVT_CHECKBOX, &TasInputWindow::OnControllerInputChanged, this);

	m_lx->Bind(wxEVT_SPINCTRL, &TasInputWindow::OnAnyInputChanged, this);
	m_ly->Bind(wxEVT_SPINCTRL, &TasInputWindow::OnAnyInputChanged, this);
	m_rx->Bind(wxEVT_SPINCTRL, &TasInputWindow::OnAnyInputChanged, this);
	m_ry->Bind(wxEVT_SPINCTRL, &TasInputWindow::OnAnyInputChanged, this);
	m_lxSlider->Bind(wxEVT_SLIDER, &TasInputWindow::OnAnyInputChanged, this);
	m_lySlider->Bind(wxEVT_SLIDER, &TasInputWindow::OnAnyInputChanged, this);
	m_rxSlider->Bind(wxEVT_SLIDER, &TasInputWindow::OnAnyInputChanged, this);
	m_rySlider->Bind(wxEVT_SLIDER, &TasInputWindow::OnAnyInputChanged, this);
	m_turboInterval->Bind(wxEVT_SPINCTRL, &TasInputWindow::OnAnyInputChanged, this);
	m_lx->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	m_ly->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	m_rx->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	m_ry->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	m_lxSlider->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	m_lySlider->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	m_rxSlider->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	m_rySlider->Bind(wxEVT_RIGHT_UP, &TasInputWindow::OnStickControlRightClick, this);
	for (auto& [_, check] : m_buttonChecks)
		check->Bind(wxEVT_CHECKBOX, &TasInputWindow::OnAnyInputChanged, this);
	resetButton->Bind(wxEVT_BUTTON, &TasInputWindow::OnResetPressed, this);

	auto bindHotkeyCapture = [this](wxWindow* control)
	{
		if (!control)
			return;
		control->Bind(wxEVT_KEY_DOWN, &TasInputWindow::OnCharHook, this);
		control->Bind(wxEVT_CHAR, &TasInputWindow::OnCharHook, this);
		control->Bind(wxEVT_CHAR_HOOK, &TasInputWindow::OnCharHook, this);
	};
	bindHotkeyCapture(m_lx);
	bindHotkeyCapture(m_ly);
	bindHotkeyCapture(m_rx);
	bindHotkeyCapture(m_ry);
	bindHotkeyCapture(m_lxSlider);
	bindHotkeyCapture(m_lySlider);
	bindHotkeyCapture(m_rxSlider);
	bindHotkeyCapture(m_rySlider);
	bindHotkeyCapture(m_turboInterval);
	bindHotkeyCapture(m_leftStickPad);
	bindHotkeyCapture(m_rightStickPad);
	for (const auto& [_, check] : m_buttonChecks)
		bindHotkeyCapture(check);
	Bind(wxEVT_CHAR_HOOK, &TasInputWindow::OnCharHook, this);
	Bind(wxEVT_TIMER, &TasInputWindow::OnStateSyncTimer, this, m_stateSyncTimer.GetId());
	m_stateSyncTimer.Start(16);

	PullStateFromTas();
	Centre();
}

void TasInputWindow::OnPlayerChanged(wxCommandEvent& event)
{
	PullStateFromTas();
	event.Skip();
}

void TasInputWindow::OnControllerInputChanged(wxCommandEvent& event)
{
	if (!m_updatingUi)
	{
		TasInput::SetControllerInputPassthroughEnabled(m_enableControllerInputCheck->GetValue());
		TasInput::SetManualInputEnabled(true);
	}
	event.Skip();
}

void TasInputWindow::OnAnyInputChanged(wxCommandEvent& event)
{
	if (m_updatingUi)
	{
		event.Skip();
		return;
	}

	m_updatingUi = true;
	const int sourceId = event.GetId();
	const bool isStickControlEvent =
		sourceId == m_lx->GetId() || sourceId == m_ly->GetId() ||
		sourceId == m_rx->GetId() || sourceId == m_ry->GetId() ||
		sourceId == m_lxSlider->GetId() || sourceId == m_lySlider->GetId() ||
		sourceId == m_rxSlider->GetId() || sourceId == m_rySlider->GetId();
	if (sourceId == m_lxSlider->GetId()) m_lx->SetValue(m_lxSlider->GetValue());
	else if (sourceId == m_lySlider->GetId()) m_ly->SetValue(255 - m_lySlider->GetValue());
	else if (sourceId == m_rxSlider->GetId()) m_rx->SetValue(m_rxSlider->GetValue());
	else if (sourceId == m_rySlider->GetId()) m_ry->SetValue(255 - m_rySlider->GetValue());

	SyncStickSlidersFromSpinControls();
	SyncStickPadsFromSpinControls();
	for (const auto& [mask, check] : m_buttonChecks)
	{
		if (!check->GetValue())
			m_turboMask &= ~mask;
	}
	RefreshTurboLabels();
	m_updatingUi = false;

	PushStateToTas();
	(void)isStickControlEvent;
	event.Skip();
}

void TasInputWindow::OnStateSyncTimer(wxTimerEvent& event)
{
	if (m_updatingUi || !TasInput::IsControllerInputPassthroughEnabled())
	{
		event.Skip();
		return;
	}
	// Avoid fighting user input while dragging controls.
	if (wxGetMouseState().LeftIsDown())
	{
		event.Skip();
		return;
	}
	PullStateFromTas();
	event.Skip();
}

void TasInputWindow::OnResetPressed(wxCommandEvent& event)
{
	m_updatingUi = true;
	m_lx->SetValue(128);
	m_ly->SetValue(128);
	m_rx->SetValue(128);
	m_ry->SetValue(128);
	m_turboMask = 0;
	for (auto& [_, check] : m_buttonChecks)
		check->SetValue(false);
	SyncStickSlidersFromSpinControls();
	SyncStickPadsFromSpinControls();
	RefreshTurboLabels();
	m_updatingUi = false;

	PushStateToTas();
	event.Skip();
}

void TasInputWindow::OnCharHook(wxKeyEvent& event)
{
	const wxEventType type = event.GetEventType();
	if ((type == wxEVT_KEY_DOWN || type == wxEVT_CHAR_HOOK) && HotkeySettings::CaptureInput(event))
	{
		event.Skip(false);
		return;
	}

	const auto isBeepProneControl = [&](wxWindow* obj) -> bool
	{
		return obj && (dynamic_cast<wxSlider*>(obj) || dynamic_cast<wxCheckBox*>(obj) || dynamic_cast<wxButton*>(obj) || dynamic_cast<wxSpinCtrl*>(obj));
	};
	auto* obj = dynamic_cast<wxWindow*>(event.GetEventObject());

	if (type == wxEVT_KEY_DOWN && isBeepProneControl(obj))
	{
		// Keep TAS hotkeys responsive while suppressing native control beeps.
		event.Skip(false);
		return;
	}

	if (type == wxEVT_CHAR)
	{
		if (isBeepProneControl(obj))
		{
			// Prevent native control beeps when a TAS hotkey is pressed while a control has focus.
			event.Skip(false);
			return;
		}
	}
	event.Skip();
}

void TasInputWindow::OnButtonRightClick(wxMouseEvent& event)
{
	auto* checkbox = dynamic_cast<wxCheckBox*>(event.GetEventObject());
	if (!checkbox)
	{
		event.Skip();
		return;
	}

	for (const auto& [mask, check] : m_buttonChecks)
	{
		if (check != checkbox)
			continue;
		if ((m_turboMask & mask) != 0)
		{
			m_turboMask &= ~mask;
			check->SetValue(false);
		}
		else
		{
			m_turboMask |= mask;
			check->SetValue(true);
		}
		break;
	}

	RefreshTurboLabels();
	PushStateToTas();
	event.Skip(false);
}

void TasInputWindow::OnStickControlRightClick(wxMouseEvent& event)
{
	auto* src = event.GetEventObject();
	if (src == m_lx || src == m_lxSlider)
		m_lx->SetValue(128);
	else if (src == m_ly || src == m_lySlider)
		m_ly->SetValue(128);
	else if (src == m_rx || src == m_rxSlider)
		m_rx->SetValue(128);
	else if (src == m_ry || src == m_rySlider)
		m_ry->SetValue(128);
	else
	{
		event.Skip();
		return;
	}

	SyncStickSlidersFromSpinControls();
	SyncStickPadsFromSpinControls();
	PushStateToTas();
	event.Skip(false);
}

void TasInputWindow::PullStateFromTas()
{
	m_updatingUi = true;
	const size_t playerIndex = (size_t)std::max(0, m_playerChoice->GetSelection());
	const auto state = TasInput::GetManualInputState(playerIndex);
	const bool controllerInputEnabled = TasInput::IsControllerInputPassthroughEnabled();
	m_enableControllerInputCheck->SetValue(controllerInputEnabled);
	TasInput::SetManualInputEnabled(true);
	m_lx->SetValue(FloatToStickByte(state.lx));
	m_ly->SetValue(FloatToStickByte(state.ly));
	m_rx->SetValue(FloatToStickByte(state.rx));
	m_ry->SetValue(FloatToStickByte(state.ry));
	m_turboMask = TasInput::GetManualTurboMask(playerIndex);
	m_turboInterval->SetValue((int)TasInput::GetManualTurboInterval(playerIndex));
	for (auto& [mask, check] : m_buttonChecks)
		check->SetValue((state.buttons & mask) != 0);
	SyncStickSlidersFromSpinControls();
	SyncStickPadsFromSpinControls();
	RefreshTurboLabels();
	m_updatingUi = false;
}

void TasInputWindow::SyncStickSlidersFromSpinControls()
{
	m_lxSlider->SetValue(m_lx->GetValue());
	m_lySlider->SetValue(255 - m_ly->GetValue());
	m_rxSlider->SetValue(m_rx->GetValue());
	m_rySlider->SetValue(255 - m_ry->GetValue());
}

void TasInputWindow::SyncStickPadsFromSpinControls()
{
	if (m_leftStickPad)
	{
		auto* leftPad = static_cast<TasStickPad*>(m_leftStickPad);
		leftPad->SetValue(StickByteToFloat(m_lx->GetValue()), StickByteToFloat(m_ly->GetValue()));
	}
	if (m_rightStickPad)
	{
		auto* rightPad = static_cast<TasStickPad*>(m_rightStickPad);
		rightPad->SetValue(StickByteToFloat(m_rx->GetValue()), StickByteToFloat(m_ry->GetValue()));
	}
	RefreshStickValueLabels();
}

void TasInputWindow::SyncStickControlsFromPad(bool leftStick, float x, float y)
{
	const int xByte = FloatToStickByte(x);
	const int yByte = FloatToStickByte(y);
	if (leftStick)
	{
		m_lx->SetValue(xByte);
		m_ly->SetValue(yByte);
	}
	else
	{
		m_rx->SetValue(xByte);
		m_ry->SetValue(yByte);
	}
	SyncStickSlidersFromSpinControls();
	RefreshStickValueLabels();
}

void TasInputWindow::RefreshStickValueLabels()
{
	if (m_leftStickValue)
		m_leftStickValue->SetLabel(wxString::Format("X: %d  Y: %d", m_lx->GetValue(), m_ly->GetValue()));
	if (m_rightStickValue)
		m_rightStickValue->SetLabel(wxString::Format("X: %d  Y: %d", m_rx->GetValue(), m_ry->GetValue()));
}

void TasInputWindow::RefreshTurboLabels()
{
	for (auto& [mask, check] : m_buttonChecks)
	{
		const wxString base = check->GetName();
		const bool turbo = (m_turboMask & mask) != 0;
		check->SetLabel(turbo ? wxString::Format("%s [T]", base) : base);
	}
}

int TasInputWindow::FloatToStickByte(float value) const
{
	const float normalized = std::clamp(value, -1.0f, 1.0f);
	const int stick = (int)std::lround(normalized * 127.0f) + 128;
	return std::clamp(stick, 0, 255);
}

float TasInputWindow::StickByteToFloat(int value) const
{
	const int stick = std::clamp(value, 0, 255);
	const float delta = (float)stick - 128.0f;
	const float normalized = delta / 127.0f;
	return std::clamp(normalized, -1.0f, 1.0f);
}

void TasInputWindow::PushStateToTas()
{
	const size_t playerIndex = (size_t)std::max(0, m_playerChoice->GetSelection());
	TasInput::ManualState state{};
	state.lx = StickByteToFloat(m_lx->GetValue());
	state.ly = StickByteToFloat(m_ly->GetValue());
	state.rx = StickByteToFloat(m_rx->GetValue());
	state.ry = StickByteToFloat(m_ry->GetValue());
	// Bias negative vertical values slightly so game-side quantization
	// rounds to the same byte shown in the TAS GUI.
	if (state.ly < 0.0f)
		state.ly = std::max(-1.0f, state.ly - (1.0f / 512.0f));
	if (state.ry < 0.0f)
		state.ry = std::max(-1.0f, state.ry - (1.0f / 512.0f));
	state.zl = 0.0f;
	state.zr = 0.0f;
	for (const auto& [mask, check] : m_buttonChecks)
	{
		if (check->GetValue())
			state.buttons |= mask;
	}

	TasInput::SetManualInputState(playerIndex, state);
	TasInput::SetManualTurboMask(playerIndex, m_turboMask);
	TasInput::SetManualTurboInterval(playerIndex, (uint32)std::max(1, m_turboInterval->GetValue()));
}

