#pragma once

#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <vector>

class TasInputWindow : public wxFrame
{
public:
	TasInputWindow(wxFrame* parent);

private:
	void OnPlayerChanged(wxCommandEvent& event);
	void OnControllerInputChanged(wxCommandEvent& event);
	void OnAnyInputChanged(wxCommandEvent& event);
	void OnStateSyncTimer(wxTimerEvent& event);
	void OnResetPressed(wxCommandEvent& event);
	void OnCharHook(wxKeyEvent& event);
	void OnButtonRightClick(wxMouseEvent& event);
	void OnStickControlRightClick(wxMouseEvent& event);

	void PullStateFromTas();
	void PushStateToTas();
	void SyncStickPadsFromSpinControls();
	void SyncStickSlidersFromSpinControls();
	void SyncStickControlsFromPad(bool leftStick, float x, float y);
	void RefreshStickValueLabels();
	void RefreshTurboLabels();
	int FloatToStickByte(float value) const;
	float StickByteToFloat(int value) const;

	wxChoice* m_playerChoice{};
	wxCheckBox* m_enableControllerInputCheck{};

	wxSpinCtrl* m_lx{};
	wxSpinCtrl* m_ly{};
	wxSpinCtrl* m_rx{};
	wxSpinCtrl* m_ry{};
	wxSlider* m_lxSlider{};
	wxSlider* m_lySlider{};
	wxSlider* m_rxSlider{};
	wxSlider* m_rySlider{};
	wxStaticText* m_leftStickValue{};
	wxStaticText* m_rightStickValue{};
	wxSpinCtrl* m_turboInterval{};
	wxPanel* m_leftStickPad{};
	wxPanel* m_rightStickPad{};
	uint32 m_turboMask{};

	std::vector<std::pair<uint32, wxCheckBox*>> m_buttonChecks;

	wxTimer m_stateSyncTimer;
	bool m_updatingUi{};
};

