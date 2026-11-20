# Patch 13 — bridge continuo (biarc AA) + gap real

## Log anterior
`fill_bridge last gap=5.89` fijo → los waypoints lerp no eran alcanzables; la cadena no era continua.

## Nuevo fill_bridge / fill_to_fixed_end
1. **Single** `fit_spline_to_end_heading` si residual &lt; 8 cm
2. **Biarc** (`compute_biarc_omsi` del AA) + 1 temp
3. **Multi** con **inicio forzado** = `evaluate(prev)` en cada nodo

Logs:
- `fill_bridge SINGLE|BIARC|MULTI gap=`
- `fill_to_fixed SINGLE|BIARC|MULTI gap=`

El gap debería caer a &lt; 0.1 m. Si no, el log lo dice.
